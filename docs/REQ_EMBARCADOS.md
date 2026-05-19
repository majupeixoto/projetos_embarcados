# Projeto Sistemas Embarcados com ESP32
**Disciplina:** Sistemas Embarcados  
**Professores:** Bella Nunes | Jymmy Barreto  

---

## 1. Objetivo do Projeto
Desenvolver um sistema IoT que integre:
* ESP32 com dispositivos sensores/atuadores.
* Comunicação sem fio (Wi-Fi) e protocolo MQTT para troca de dados.
* Dashboard/App para visualização, interação e gestão em tempo real dos dados coletados.

---

## 2. Requisitos Técnicos

### Hardware Obrigatório
* 1x ou 2x módulos ESP32 (NodeMCU) - Pode usar ESP8266 como módulo secundário.
* Sensores básicos (ex.: DHT11/DHT22, LM35, LDR, PIR, etc.).
* Componentes eletrônicos (resistor, LED, jumper, etc.).

Cada grupo receberá um kit de projeto contendo os seguintes componentes:

| Quantidade | Componente |
| :--- | :--- |
| 1 | ESP32 |
| 1 | LED RGB |
| 1 | Sensor Ultrassônico |
| 1 | Sensor Infravermelho |
| 2 | Protoboards |
| 1 | Maleta |
| 1 | Potenciômetro |
| - | LEDs (vermelho, amarelo e/ou verde) |
| - | Resistores |
| - | Jumpers |
| - | Botões |

Além desses componentes, é possível acessar os recursos do Laboratório Garagem. Caso não haja estoque de determinado componente, os estudantes deverão providenciar com recurso próprio.

### Software/Ferramentas
* **Broker MQTT**
  * ESP32: Um módulo ESP32;
  * Raspberry Pi: Sistema Operacional - Raspberry Pi OS (Linux) ou Raspibian (Debian ARM);
  * Mosquitto instalado no próprio PC (Máquina Virtual, Contêiner ou WSL);
* **Aplicação Web** (Python + Flask/Django, JavaScript, TypeScript, NodeJS, React, Angular, Banco de Dados etc).
* **VSCode Platform I/O:**
  * Firmware (FreeRTOS);
  * Bibliotecas: MQTT, WI-FI, Bluetooth, RF entre outras que podem ser necessárias.
* **Versionamento:** GitHub (repositório público).

Será disponibilizado, caso um ou mais grupos tenha interesse, um Learner Labs da AWS Academy, que oferece muitas funcionalidades e ferramentas da AWS que podem ser utilizadas para o desenvolvimento do projeto. Exemplos das ferramentas/recursos que são oferecidos:
* EC2, S3, EBS e EFS/FSx
* IoT CORE
* Lambda
* Amazon RDS, DynamoDB, Amazon Redshift

Será disponibilizado quarenta dólares ($40) por estudante que será utilizado no custeio dos serviços/recursos utilizados.

---

## 3. Critérios de Avaliação

| Item | Pontuação | Detalhes |
| :--- | :--- | :--- |
| **Check Point (26/05)** | 5 pts | Demonstração e pequenas entregas do projeto. |
| **Funcionamento do Sistema** | 25 pts | Projeto operando conforme requisitos (comunicação MQTT, Protocolo IPv6 coleta de dados, dashboard). |
| **Dashboard** | 20 pts | Interface visual funcional, exibindo dados em tempo real. |
| **Relatório Técnico (MNR/ABNT2)** | 20 pts | Estrutura completa: introdução, metodologia, resultados, códigos em apêndice. |
| **GitHub (Organização)** | 10 pts | Repositório bem documentado, commits colaborativos, README detalhado. |
| **Apresentação (20/06)** | 20 pts | Clareza, demonstração ao vivo, divisão de tarefas no grupo. |

**Nota 1 :** Projetos com funcionalidades extras (ex.: Alertas por e-mail ou Apps de mensagens instantâneas, Infraestrutura de comunicação em IPv6) ganharão bônus de até 10 pts.

---

## 4. Template do Repositório GitHub
Exemplo da Estrutura:

```text
├── README.md         # Descrição do projeto, requisitos, instruções.
├── /docs             # Relatório em PDF (MNR - ABNT2) + imagens.
├── /applications     # Códigos Backend + Frontend + Outros.
├── /esp32-esp8266    # Firmware dos módulos (FreeRTOS).
└── /schematics       # Diagramas eletrônicos (Tinkercad, Fritzing, Wokwi, KiCad ou outra ferramenta de prototipação).