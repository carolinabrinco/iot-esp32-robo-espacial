# Robô Explorador Espacial — Dashboard IoT

Dashboard de telemetria para o robô ESP32, com API Flask e persistência SQLite. Projeto acadêmico SENAI CIMATEC — Camada de Serviço.

## Executar o backend e o dashboard

```bash
python -m venv .venv
# Windows: .venv\Scripts\activate
# Linux/macOS: source .venv/bin/activate
pip install -r requirements.txt
python app.py
```

Abra `http://localhost:5000`. O banco `leituras.db` é criado automaticamente usando `schema.sql`.

## Enviar uma leitura

```bash
curl -X POST http://localhost:5000/leituras -H "Content-Type: application/json" -d '{"temperatura_c":24.3,"umidade_pct":55,"luminosidade":723,"presenca":1,"probabilidade_vida":78}'
```

A API aceita `timestamp` opcional em ISO 8601. O robô deve enviar uma leitura a cada 2 segundos.

## Consultar dados salvos

- `GET /leituras`: últimas 100 leituras, em JSON, da mais recente para a mais antiga.
- `GET /leituras?limit=30`: últimas 30 leituras.
- `GET /`: dashboard com atualização automática a cada 2 segundos.

## Montagem do robô

Conforme o edital: conectar ao ESP32 físico dois motores DC **com driver de motor e alimentação apropriada**, sensor DHT de temperatura e umidade, fotorresistor, sensor PIR, LED verde e LED vermelho. O ESP32 do controle remoto no Wokwi usa joystick, botão de desligamento e LEDs de status. **As ligações GPIO e o firmware físico devem ser confirmados com a montagem real antes de energizar.** Nunca ligar motores diretamente aos GPIOs.

## Organização e entregáveis

- `app.py`: API Flask e dashboard.
- `schema.sql`: criação da tabela de leituras.
- `templates/index.html`, `static/`: interface.
- `requirements.txt`: dependências.

**Pendente para completar a Etapa 04 do edital:** incorporar os códigos `.ino` originais do controle remoto e do robô físico e o link do Wokwi. Não foram incluídos firmwares fictícios.

## Observações

A probabilidade de vida é recebida do ESP32, não calculada pelo dashboard. O alerta visual aparece quando o valor é superior a 75%. O dashboard exibe dados persistidos, mas não afirma conexão física em tempo real com o robô. Para acesso de outros dispositivos, configurar rede e segurança adequadas; a API de exemplo não possui autenticação.

## Firmware recebido
`firmware/monitoramento_esp32s3.ino` é o código fornecido do ESP32-S3 de monitoramento via ESP-NOW, matriz MAX7219 e envio HTTP. Ele não substitui o firmware do robô com motores e joystick, ainda não fornecido. Configure Wi-Fi e URL da API localmente.

## Entrega da Etapa 04
Use a branch `feat/robo-lab`. Ainda é necessário adicionar o firmware específico do robô para cumprir todos os itens do edital.
