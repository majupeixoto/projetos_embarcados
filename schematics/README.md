# schematics/

Esta pasta armazena os esquemas de ligação elétrica do protótipo VitaLink.

## Conexões principais

| Componente | Pino ESP32 | Observação |
|---|---|---|
| MPU-6500 SDA | GPIO 21 | I2C dados |
| MPU-6500 SCL | GPIO 22 | I2C clock |
| MPU-6500 VCC | 3.3 V | **Não** conectar ao 5 V |
| MPU-6500 GND | GND | AD0 ao GND → endereço 0x68 |
| LED vermelho (R) | GPIO 25 | Resistor 220 Ω em série |
| LED verde (G) | GPIO 26 | Resistor 220 Ω em série |
| LED azul (B) | GPIO 27 | Resistor 220 Ω em série |
| Botão pânico | GPIO 15 | Pull-up interno — outro terminal ao GND |

## Arquivos esperados

- `vitalink_fritzing.fzz` — diagrama Fritzing para protoboard
- `vitalink_schematic.pdf` — esquema elétrico exportado

Adicione os arquivos acima nesta pasta antes da entrega final.
