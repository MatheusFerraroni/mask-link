# ESP32 Wi-Fi NAT Router

Projeto PlatformIO para ESP32-WROOM-32D usando ESP-IDF. O firmware cria um SoftAP de configuração, conecta a interface STA a uma rede Wi-Fi externa e habilita NAPT para os clientes conectados ao AP do ESP32.

Quando a STA recebe IP, ela se torna a interface padrão, e o NAT é habilitado na interface AP com `esp_netif_napt_enable`.

## Padrões

* AP SSID: `ESP32-Config`
* Senha do AP: `12345678` fixa
* IP do AP: `192.168.4.1`
* DHCP do AP: `192.168.4.2` até `192.168.4.50`
* Máximo de clientes no AP: `4`
* DNS entregue aos clientes do AP: `8.8.8.8`
* LED de estado: GPIO 2, ativo em nível alto

As configurações ficam na NVS e não são apagadas ao reiniciar. Alterações no SSID do AP, no número máximo de clientes, no IP do AP e na faixa DHCP são salvas e aplicadas após o reboot.

## Build

```sh
/Users/matheus/.platformio/penv/bin/pio run
```

Caso `pio` esteja no `PATH`:

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
2. Conecte o celular ou computador ao Wi-Fi `ESP32-Config` com a senha `12345678`.
3. Abra `http://192.168.4.1`.
4. Em `Rede externa`, busque redes ou digite o SSID e a senha manualmente.
5. Salve. O ESP32 mantém o AP ativo enquanto conecta como STA.
6. Quando a STA recebe IP, o NAT fica ativo, e os clientes do AP navegam pela internet passando pelo ESP32.

O firmware anuncia a URL de configuração via captive portal DHCP e redireciona requisições HTTP desconhecidas para a tela. A abertura automática da tela depende do sistema operacional. Caso ela não abra automaticamente, acesse `http://192.168.4.1` manualmente.

## Painel web

A tela principal inclui:

* status completo: uptime, heap livre, estado da STA, SSID externo, IP/gateway/DNS da STA, RSSI, NAT, internet e quantidade de clientes;
* formulário da rede externa com senha salva visível;
* scanner Wi-Fi sob demanda; tocar ou clicar em um SSID preenche o campo da rede externa;
* configuração do AP: SSID, número máximo de clientes, IP do AP e faixa DHCP;
* configuração do DNS entregue aos clientes do AP;
* lista atualizada automaticamente de clientes conectados com MAC e IP;
* teste manual de internet com etapas STA, ping `8.8.8.8`, DNS `example.com` e HTTP `http://example.com`;
* logs em RAM com os últimos 30 eventos do uptime atual;
* botões para esquecer o Wi-Fi externo, reiniciar e resetar tudo para o padrão.

## Endpoints

* `GET /`: painel web.
* `GET /status`: JSON com status, clientes e eventos.
* `GET /scan`: scan Wi-Fi sob demanda.
* `POST /save`: salva o SSID e a senha da rede externa e conecta a STA.
* `POST /forget`: apaga as credenciais da rede externa.
* `POST /dns`: salva o DNS customizado dos clientes do AP.
* `POST /dns/reset`: volta o DNS para `8.8.8.8`.
* `POST /ap-config`: salva a configuração do AP e reinicia.
* `POST /connectivity/test`: executa teste manual de internet.
* `POST /reboot`: reinicia o ESP32.
* `POST /factory-reset`: apaga credenciais, DNS e configurações do AP, voltando aos padrões.

## LED de estado

| Estado                               | Funcionamento                                                   |
| ------------------------------------ | --------------------------------------------------------------- |
| Pronto para configurar               | 2 s ligado, 0,3 s desligado                                     |
| Conectando ao Wi-Fi                  | 0,5 s ligado, 0,5 s desligado                                   |
| Funcionando normalmente              | Ligado fixo                                                     |
| Sem internet                         | 2 piscadas rápidas, pausa de 2 s                                |
| Cliente conectado ao AP              | Liga por 1 s, faz 5 piscadas rápidas e volta ao estado anterior |
| Salvando configuração ou reiniciando | Piscadas rápidas contínuas                                      |
| Resetando para o padrão de fábrica   | Piscadas rápidas por 3 s, depois 2 piscadas longas              |
| Erro que precisa de atenção          | 3 piscadas rápidas, pausa longa                                 |

## Checagem automática

Uma tarefa roda a cada 10 segundos e faz ping para `8.8.8.8`. Quando o estado da internet muda, um evento é adicionado ao log em memória, e o LED alterna entre funcionamento normal e sem internet.

## Configuração do AP

Validações aplicadas antes de salvar:

* SSID do AP obrigatório;
* máximo de clientes entre `1` e `10`;
* IP do AP, DHCP inicial e DHCP final precisam estar no mesmo `/24`;
* IP do AP não pode estar dentro da faixa DHCP;
* DHCP inicial precisa ser menor ou igual ao DHCP final.

Máscara do AP fixa: `255.255.255.0`.

## sdkconfig.defaults

O projeto ativa NAT e roteamento IPv4:

```ini
CONFIG_LWIP_IP_FORWARD=y
CONFIG_LWIP_IPV4_NAPT=y
```
