#pragma once
#include <stddef.h>   // size_t
#include <stdint.h>

// ═══════════════════════════════════════════════════════════════════════════════
// CircularBuffer<T, CAP>
// ═══════════════════════════════════════════════════════════════════════════════
//
// Ring buffer de capacidade fixa CAP, inteiramente alocado em tempo de compilação.
//
// PRINCÍPIO DE FUNCIONAMENTO
// ──────────────────────────
// O array interno _buf[CAP] é circular: os índices HEAD e TAIL avançam com
// aritmética modular (% CAP) sem nunca deslocar elemento algum.
//
//   Escrita (push): grava em _buf[_head], depois _head = (_head + 1) % CAP
//   Leitura (pop) : lê  de _buf[_tail], depois _tail = (_tail + 1) % CAP
//
// Estado invariante: _count conta os elementos válidos entre TAIL e HEAD.
//
//   TAIL ──read──▶ [e0][e1][e2][ ][ ] ◀──write── HEAD
//                   0   1   2  3  4
//                  _count = 3,  CAP = 5
//
// COMPLEXIDADE ASSINTÓTICA
// ────────────────────────
//   push()     → O(1)  — uma atribuição + um incremento modular
//   pop()      → O(1)  — uma leitura   + um incremento modular
//   peek()     → O(1)  — uma leitura
//   isEmpty()  → O(1)  — comparação de inteiro
//   isFull()   → O(1)  — comparação de inteiro
//   size()     → O(1)  — leitura de membro
//   clear()    → O(1)  — três atribuições de inteiro
//
// Nenhuma operação percorre o array, portanto o tempo de execução é
// INDEPENDENTE de CAP e de _count. Isso elimina o jitter de inserção.
//
// ALOCAÇÃO DE MEMÓRIA
// ───────────────────
// O array _buf[CAP] é um membro de valor da struct → sem heap, sem malloc,
// sem fragmentação. Quando a instância é global/static, o array vai para o
// segmento BSS (inicializado em zero antes de main() pelo runtime do ESP32).
//
// POLÍTICA DE OVERWRITE
// ─────────────────────
// Quando cheio (isFull()), push() sobrescreve o elemento mais antigo (TAIL
// avança junto com HEAD). Adequado para telemetria contínua onde os dados
// mais recentes têm prioridade sobre os mais antigos.
// Para rejeitar novas inserções quando cheio, troque o if(full) por return false.
//
// USO TÍPICO
// ──────────
//   CircularBuffer<float, 512> buf;   // 512 floats na BSS — sem heap
//   buf.push(leituraSensor());        // O(1)
//   float val;
//   if (buf.pop(val)) processar(val); // O(1)
//
// ═══════════════════════════════════════════════════════════════════════════════

template<typename T, size_t CAP>
class CircularBuffer {

    static_assert(CAP > 0, "CircularBuffer: CAP deve ser maior que zero.");

public:

    // ── Construtor ────────────────────────────────────────────────────────────
    // Nenhuma alocação dinâmica. Inicializa apenas os três índices de controle.
    CircularBuffer() : _head(0), _tail(0), _count(0) {}

    // ── push ──────────────────────────────────────────────────────────────────
    // Insere 'value' na posição HEAD e avança HEAD com módulo.
    //
    // Se o buffer estiver cheio: TAIL avança junto (descarta o elemento mais
    // antigo) e a função retorna false para sinalizar o overwrite.
    //
    // Complexidade: O(1) — uma atribuição de elemento + dois incrementos modulares
    bool push(const T& value) {
        bool full = isFull();
        _buf[_head] = value;
        _head = (_head + 1) % CAP;
        if (full) {
            // Overwrite: HEAD alcançou TAIL → avança TAIL para liberar espaço
            // Sem memmove, sem cópia: apenas aritmética de índice.
            _tail = (_tail + 1) % CAP;
            return false;    // sinaliza: um elemento antigo foi descartado
        }
        _count++;
        return true;         // inserção limpa
    }

    // ── pop ───────────────────────────────────────────────────────────────────
    // Remove e retorna o elemento mais antigo (TAIL) via 'out'.
    // Retorna false se o buffer estiver vazio (out não é modificado).
    //
    // Complexidade: O(1) — uma leitura de elemento + um incremento modular
    bool pop(T& out) {
        if (isEmpty()) return false;
        out = _buf[_tail];
        _tail = (_tail + 1) % CAP;
        _count--;
        return true;
    }

    // ── peek ──────────────────────────────────────────────────────────────────
    // Lê o elemento mais antigo sem removê-lo.
    // Complexidade: O(1)
    bool peek(T& out) const {
        if (isEmpty()) return false;
        out = _buf[_tail];
        return true;
    }

    // ── clear ─────────────────────────────────────────────────────────────────
    // Reseta o buffer sem zerar o array (apenas reposiciona os índices).
    // Complexidade: O(1) — três atribuições de inteiro
    void clear() { _head = _tail = _count = 0; }

    // ── predicados e metadados ────────────────────────────────────────────────
    // Todos O(1) — sem iteração.
    bool   isEmpty()   const { return _count == 0;    }
    bool   isFull()    const { return _count == CAP;  }
    size_t size()      const { return _count;          }
    size_t capacity()  const { return CAP;             }

    // ── Impede cópia acidental ────────────────────────────────────────────────
    // O array interno tem CAP elementos; cópia implicaria O(CAP) — use referências.
    CircularBuffer(const CircularBuffer&)            = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;

private:
    T      _buf[CAP];   // array estático: tamanho resolvido em tempo de compilação
    size_t _head;       // próxima posição de ESCRITA  (produção)
    size_t _tail;       // próxima posição de LEITURA  (consumo)
    size_t _count;      // elementos válidos presentes no buffer
};
