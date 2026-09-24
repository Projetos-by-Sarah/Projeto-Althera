# Althera – Firmware da fonte de tensão inteligente

Firmware para **ESP32-S3-WROOM-2-N16R8** (PlatformIO + Arduino) que faz três coisas:

1. **Controla a corrente de carga da bateria** (2 A, 4 A ou 10 A) por software.
2. **Monitora tensão, corrente e potência** de todas as entradas e saídas usando 4× INA3221.
3. **Expõe uma API HTTP/JSON** para o dashboard na nuvem conversar com cada placa pelo IP.

> Status: firmware ainda não testado em hardware. Use o modo debug serial (abaixo) para validar a placa antes de subir qualquer coisa pra nuvem.

---

## Estrutura

```
althera-firmware/
├── platformio.ini      # placa, flash/PSRAM octal, libs
├── include/
│   ├── config.h        # WiFi, pinos, modo debug, intervalos
│   └── ina3221.h       # driver mínimo do INA3221 (sem biblioteca externa)
└── src/
    └── main.cpp        # carga, leitura dos INAs, API, comandos serial
```

## Mapa de hardware

| Função | Pino |
|---|---|
| I2C SDA | GPIO 8 |
| I2C SCL | GPIO 9 |
| CHG_4A | GPIO 41 |
| CHG_10A | GPIO 40 |

No WROOM-2 N16R8 (flash e PSRAM octal) os GPIO 35, 36 e 37 são reservados. Os pinos acima estão livres.

### INA3221 (pino A0 define o endereço)

| CI | A0 em | Endereço | Canais |
|---|---|---|---|
| U27 | GND | 0x40 | IN1 Bateria, IN2 Solar, IN3 Fonte |
| U26 | VS | 0x41 | IN1 24V, IN2 12V CH1, IN3 12V CH2 |
| U25 | SDA | 0x42 | IN1 12V CH3, IN2 12V CH4, IN3 12V CH5 |
| U28 | SCL | 0x43 | IN1 5V_1, IN2 5V_2, IN3 9V (**confirmar no esquemático**) |

Resistores shunt: entradas 5 mΩ/3 W, 12 V 5 mΩ/2 W, 24 V 10 mΩ/1 W, 5 V e 9 V 10 mΩ/1 W.
Cada canal tem seu `r_mOhm` na tabela `channels[]` do `main.cpp`.

---

## Como funciona

### 1. Carga programável da bateria

Os dois GPIOs alteram o resistor de sense do carregador:

| Modo | GPIO41 | GPIO40 | Rsense | Corrente |
|---|---|---|---|---|
| `slow` (base) | LOW | LOW | só R21 (20 mΩ) | **2 A** |
| `medium` | HIGH | LOW | 20 mΩ ‖ 20 mΩ = 10 mΩ | **4 A** |
| `fast` | LOW | HIGH | 20 mΩ ‖ 5 mΩ = 4 mΩ | **10 A** |
| ~~proibido~~ | HIGH | HIGH | 3,33 mΩ | ~12 A (fora do projeto) |

Proteções no código:
- No boot os pinos sobem em **LOW** antes de qualquer outra coisa, então a placa começa em 2 A.
- Toda troca de modo passa por LOW/LOW antes de subir o novo pino. **HIGH+HIGH nunca é aplicado**.
- O modo **não é salvo** na memória: reiniciou, volta pra 2 A.

### 2. Leitura dos INAs

A cada `SAMPLE_INTERVAL_MS` (500 ms) o firmware lê todos os canais:

- Tensão de barramento: `V = raw × 8 mV`
- Tensão no shunt: `Vshunt = raw × 40 µV`
- Corrente: `I = Vshunt(mV) / R(mΩ)` (resultado em A)
- Potência: `P = V × I`

Os INAs são configurados com média de 16 amostras e conversão de 1,1 ms. No boot o firmware confere o *Manufacturer ID* (0x5449) de cada chip. Se um não responder, ele fica marcado como offline e o firmware tenta de novo a cada 5 s, sem travar o resto.

Cada canal tem um `role` (fonte, bateria ou carga), usado para somar os totais:
`sources_w` (fonte + solar), `loads_w` (todas as saídas) e `battery_w`.

O campo `sign` do canal vale `+1` ou `-1`. Se um shunt estiver montado ao contrário, troque o `sign` dele.

### 3. API HTTP

Porta 80, CORS liberado (inclusive `Access-Control-Allow-Private-Network`), também acessível por `http://<DEVICE_ID>.local`.

| Método | Rota | Função |
|---|---|---|
| GET | `/api/status` | todas as medições em JSON |
| GET | `/api/charge` | modo de carga atual |
| POST | `/api/charge` | troca o modo de carga |

```bash
curl http://192.168.0.50/api/status

curl -X POST http://192.168.0.50/api/charge \
     -H "Content-Type: application/json" \
     -d '{"mode":"medium"}'        # slow | medium | fast   (ou {"amps":4})
```

Se `API_TOKEN` no `config.h` não for vazio, o POST exige o header `X-Api-Key: <token>`.

Exemplo de resposta do `/api/status` (resumido):

```json
{
  "device": "althera-01",
  "uptime_s": 1234,
  "charge": { "mode": "slow", "current_a": 2 },
  "chips": [ { "ref": "U27", "addr": "0x40", "online": true } ],
  "channels": [
    { "name": "BATERIA", "role": "battery", "chip": "U27", "valid": true, "v": 12.6, "i": 1.98, "p": 24.95 }
  ],
  "totals": { "sources_w": 30.1, "loads_w": 22.4, "battery_w": 24.95 }
}
```

> Se o dashboard estiver em `https://`, o navegador bloqueia chamadas para `http://IP-da-placa` (mixed content). Hospede o dashboard em HTTP ou troque a API direta por MQTT.

---

## Modo debug de bancada (sem WiFi)

No `include/config.h`:

```c
#define WIFI_ENABLED  0   // sem WiFi/servidor: boot instantâneo, só serial
#define LOG_MODE      1   // 1 tabela | 2 gráfico Teleplot | 3 CSV
#define LOG_INTERVAL_MS 1000
```

**Comandos pelo monitor serial** (funcionam com ou sem WiFi): `s` = 2 A, `m` = 4 A, `f` = 10 A.

### Gráfico ao vivo no VS Code

1. Instale a extensão **Teleplot** (Alberto Gonzalez).
2. Ponha `LOG_MODE 2` e grave o firmware.
3. Feche o monitor serial do PlatformIO (a porta só pode estar aberta em um lugar).
4. Abra o Teleplot pelo ícone na barra lateral, escolha a porta serial (115200) e conecte.

Cada canal vira três curvas (`NOME_V`, `NOME_A`, `NOME_W`), mais `carga_A` com o modo de carga atual.

### Checklist de bancada

1. Com `LOG_MODE 1`, veja no boot se aparece `U27/U26/U25/U28 OK`. Se algum aparecer `OFFLINE`, confira o endereço (pino A0), o pull-up do I2C e a alimentação.
2. Com carga conhecida (multímetro em série), compare a corrente lida com a real.
3. Corrente negativa onde deveria ser positiva: inverta o `sign` do canal.
4. Com a bateria conectada, teste `s`, `m` e `f` e confira a corrente de carga com o multímetro.

---

## Configuração inicial

1. Edite `include/config.h`: `WIFI_SSID`, `WIFI_PASS`, `DEVICE_ID` (único por placa) e `API_TOKEN`.
2. Ajuste os nomes dos canais do U28 em `channels[]`, conforme o esquemático.
3. Build (`Ctrl+Alt+B`) e Upload pelo PlatformIO.
4. Monitor serial em 115200 baud.

## Pendências / ideias

- Confirmar a ordem dos canais do U28 (5 V e 9 V).
- Salvar o modo de carga em NVS (hoje volta pra 2 A ao reiniciar).
- Limites de alarme (sobrecorrente, subtensão da bateria) com corte automático da carga rápida.
- Dashboard HTML na nuvem consumindo `/api/status` de cada placa.