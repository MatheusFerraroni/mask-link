# ESP32 Wi-Fi NAT Router

Projeto PlatformIO para ESP32-WROOM-32D usando ESP-IDF. O firmware cria um SoftAP de configuracao, conecta a interface STA em uma rede Wi-Fi externa e habilita NAPT para os clientes conectados ao AP do ESP32.

Quando a STA recebe IP, ela vira a interface default e o NAT e habilitado na interface AP com `esp_netif_napt_enable`.

## Padroes

- AP SSID: `ESP32-Config`
- AP senha: `12345678` fixa
- AP IP: `192.168.4.1`
- DHCP AP: `192.168.4.2` ate `192.168.4.50`
- Maximo de clientes AP: `4`
- DNS entregue aos clientes AP: `8.8.8.8`
- LED de estado: GPIO 2, ativo em nivel alto

As configuracoes ficam em NVS e nao sao apagadas ao reiniciar. Alteracoes de SSID do AP, maximo de clientes, IP do AP e faixa DHCP sao salvas e aplicadas apos reboot.

## Build

```sh
/Users/matheus/.platformio/penv/bin/pio run
```

Se `pio` estiver no `PATH`:

```sh
pio run
```

## Upload

```sh
/Users/matheus/.platformio/penv/bin/pio run --target upload
```

Monitor serial:

```sh
/Users/matheus/.platformio/penv/bin/pio device monitor --baud 115200
```

## Como usar

1. Grave o firmware no ESP32.
2. Conecte o celular/computador no Wi-Fi `ESP32-Config` com senha `12345678`.
3. Abra `http://192.168.4.1`.
4. Em `Rede externa`, busque redes ou digite SSID/senha manualmente.
5. Salve. O ESP32 mantem o AP ativo enquanto conecta como STA.
6. Quando a STA receber IP, o NAT fica ativo e os clientes do AP navegam pela internet passando pelo ESP32.

O firmware anuncia a URL de configuracao via captive portal DHCP e redireciona requisicoes HTTP desconhecidas para a tela. A abertura automatica da tela depende do sistema operacional; se nao abrir sozinha, acesse `http://192.168.4.1` manualmente.

## Painel web

A tela principal inclui:

- status completo: uptime, heap livre, estado STA, SSID externo, IP/gateway/DNS da STA, RSSI, NAT, internet e quantidade de clientes;
- formulario da rede externa com senha salva visivel;
- scanner Wi-Fi sob demanda; tocar/clicar em um SSID preenche o campo da rede externa;
- configuracao do AP: SSID, maximo de clientes, IP do AP e faixa DHCP;
- configuracao do DNS entregue aos clientes do AP;
- lista atualizada automaticamente de clientes conectados com MAC e IP;
- teste manual de internet com etapas STA, ping `8.8.8.8`, DNS `example.com` e HTTP `http://example.com`;
- logs em RAM com os ultimos 30 eventos do uptime atual;
- botoes para esquecer Wi-Fi externo, reiniciar e resetar tudo para padrao.

## Endpoints

- `GET /`: painel web.
- `GET /status`: JSON com status, clientes e eventos.
- `GET /scan`: scan Wi-Fi sob demanda.
- `POST /save`: salva SSID/senha da rede externa e conecta a STA.
- `POST /forget`: apaga as credenciais da rede externa.
- `POST /dns`: salva DNS customizado dos clientes AP.
- `POST /dns/reset`: volta o DNS para `8.8.8.8`.
- `POST /ap-config`: salva configuracao do AP e reinicia.
- `POST /connectivity/test`: executa teste manual de internet.
- `POST /reboot`: reinicia o ESP32.
- `POST /factory-reset`: apaga credenciais, DNS e configuracoes do AP, voltando aos padroes.

## LED de estado

| Estado | Funcionamento |
| --- | --- |
| Pronto para configurar | 2 s ligado, 0,3 s desligado |
| Conectando ao Wi-Fi | 0,5 s ligado, 0,5 s desligado |
| Funcionando normalmente | Ligado fixo |
| Sem internet | 2 piscadas rapidas, pausa de 2 s |
| Cliente conectou ao AP | Liga por 1 s, faz 5 piscadas rapidas e volta ao estado anterior |
| Salvando configuracao ou reiniciando | Piscadas rapidas continuas |
| Resetando para padrao de fabrica | Piscadas rapidas por 3 s, depois 2 piscadas longas |
| Erro que precisa de atencao | 3 piscadas rapidas, pausa longa |

## Checagem automatica

Uma tarefa roda a cada 10 segundos e faz ping para `8.8.8.8`. Quando o estado de internet muda, um evento e adicionado ao log em memoria e o LED muda entre funcionamento normal e sem internet.

## Configuracao do AP

Validacoes aplicadas antes de salvar:

- SSID do AP obrigatorio;
- maximo de clientes entre `1` e `10`;
- IP do AP, DHCP inicio e DHCP fim precisam estar no mesmo `/24`;
- IP do AP nao pode estar dentro da faixa DHCP;
- DHCP inicio precisa ser menor ou igual ao DHCP fim.

Mascara do AP fixa: `255.255.255.0`.

## sdkconfig.defaults

O projeto ativa NAT e roteamento IPv4:

```ini
CONFIG_LWIP_IP_FORWARD=y
CONFIG_LWIP_IPV4_NAPT=y
```
