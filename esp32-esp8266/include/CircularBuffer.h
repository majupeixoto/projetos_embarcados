#pragma once
#include <stddef.h>   // size_t
#include <stdint.h>

// COMPLEXIDADE ASSINTÓTICA
// ────────────────────────
//   push()     → O(1)  — uma atribuição + um incremento modular
//   pop()      → O(1)  — uma leitura   + um incremento modular
//   peek()     → O(1)  — uma leitura
//   isEmpty()  → O(1)  — comparação de inteiro
//   isFull()   → O(1)  — comparação de inteiro
//   size()     → O(1)  — leitura de membro
//   clear()    → O(1)  — três atribuições de inteiro

template<typename T, size_t CAP>
class CircularBuffer {

    static_assert(CAP > 0, "CircularBuffer: CAP deve ser maior que zero.");

public:
    CircularBuffer() : _head(0), _tail(0), _count(0) {}
    //push 
    // Insere 'value' na posição HEAD e avança HEAD com módulo.
    // Se o buffer estiver cheio: TAIL avança junto (descarta o elemento mais
    // Complexidade: O(1) — uma atribuição de elemento + dois incrementos modulares
    bool push(const T& value) {
        bool full = isFull();
        _buf[_head] = value;
        _head = (_head + 1) % CAP;
        if (full) {
            // Overwrite: HEAD alcançou TAIL → avança TAIL para liberar espaço
            // Sem memmove, sem cópia: apenas aritmética de índice.
            _tail = (_tail + 1) % CAP;
            return false;
        }
        _count++;
        return true;
    }

    //pop
    // Remove e retorna o elemento mais antigo (TAIL) via 'out'.
    // Retorna false se o buffer estiver vazio (out não é modificado).
    // Complexidade: O(1) — uma leitura de elemento + um incremento modular
    bool pop(T& out) {
        if (isEmpty()) return false;
        out = _buf[_tail];
        _tail = (_tail + 1) % CAP;
        _count--;
        return true;
    }

    //peek
    // Lê o elemento mais antigo sem removê-lo.
    // Complexidade: O(1)
    bool peek(T& out) const {
        if (isEmpty()) return false;
        out = _buf[_tail];
        return true;
    }

    //clear
    // Reseta o buffer sem zerar o array (apenas reposiciona os índices).
    // Complexidade: O(1) — três atribuições de inteiro
    void clear() { _head = _tail = _count = 0; }

    // predicados e metadados
    // Todos O(1) — sem iteração.
    bool   isEmpty()   const { return _count == 0;    }
    bool   isFull()    const { return _count == CAP;  }
    size_t size()      const { return _count;          }
    size_t capacity()  const { return CAP;             }

    CircularBuffer(const CircularBuffer&)            = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;

private:
    T      _buf[CAP];   // array estático: tamanho resolvido em tempo de compilação
    size_t _head;       // próxima posição de ESCRITA  (produção)
    size_t _tail;       // próxima posição de LEITURA  (consumo)
    size_t _count;      // elementos válidos presentes no buffer
};
