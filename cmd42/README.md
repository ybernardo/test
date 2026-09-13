# Raspberry Pi 4 — kernel e utilitário CMD42 para cartões SD

Esta branch contém uma compilação reproduzível e experimental do kernel
`6.18.39+rpt-rpi-v8` usado no Raspberry Pi OS Debian 13 (Trixie).

O patch altera a inicialização do subsistema MMC para manter um cartão SD
protegido por senha registrado no sistema. Isso permite que o utilitário
`sd-cmd42` envie o comando nativo SD `CMD42` pelo leitor microSD integrado
da Raspberry Pi 4.

## Conteúdo

- `patches/expose-locked-sd.patch`: alteração mínima em
  `drivers/mmc/core/sd.c`.
- `sd-cmd42.c`: utilitário para definir, bloquear, desbloquear, remover a
  senha e executar Force Erase.
- `cmd42-rescue`: menu textual que detecta o cartão SD nativo e conduz as
  operações sem exigir que o usuário memorize os comandos.
- `install-rescue.sh`: instalação segura do kernel e dos utilitários, com
  backups e validação da Raspberry Pi 4.
- `build-kernel.sh`: baixa os pacotes oficiais exatos da Raspberry Pi,
  recupera a configuração do kernel oficial e faz cross-compilation ARM64.
- `.github/workflows/build-cmd42-kernel.yml`: executa o build no GitHub
  Actions e publica o `Image.gz`, a configuração, o patch e checksums.
- `.github/workflows/build-cmd42-rescue-image.yml`: gera, sob demanda, uma
  imagem limpa e compactada do Raspberry Pi OS Lite com o kernel e o menu já
  instalados.

## Compilar o utilitário

```bash
gcc -O2 -Wall -Wextra -Werror -std=gnu11 \
  -o sd-cmd42 sd-cmd42.c
sudo install -m 0755 sd-cmd42 /usr/local/sbin/sd-cmd42
```

## Operações

Na imagem de recuperação, o modo mais simples é abrir o menu:

```bash
sudo cmd42-rescue
```

Para uso direto ou automação, utilize a CLI:

```text
sudo sd-cmd42 set      /dev/mmcblkN [NOVA_SENHA]
sudo sd-cmd42 lock     /dev/mmcblkN [SENHA] --confirm-lock
sudo sd-cmd42 set-lock /dev/mmcblkN [NOVA_SENHA] --confirm-lock
sudo sd-cmd42 unlock   /dev/mmcblkN
sudo sd-cmd42 clear    /dev/mmcblkN
sudo sd-cmd42 erase    /dev/mmcblkN --confirm-erase
```

Quando omitidas, as senhas são solicitadas sem eco. Os comandos também aceitam
a senha como argumento para automação e compatibilidade, mas isso não é recomendado porque
ela pode ficar registrada no histórico do shell e visível na lista de
processos.

A senha SD deve possuir de 1 a 16 bytes. Para evitar incompatibilidades, o
utilitário aceita somente caracteres ASCII imprimíveis.

## Escopo e segurança

- `set` grava uma senha persistente sem bloquear a sessão atual.
- `lock` bloqueia imediatamente um cartão que já possui senha.
- `set-lock` grava a senha e bloqueia imediatamente.
- `unlock` preserva os dados, mas o desbloqueio dura somente até o cartão
  perder alimentação.
- `clear` remove a senha permanentemente sem apagar os dados. Se o cartão
  estiver bloqueado, o próprio comando o desbloqueia internamente antes de
  remover a senha; não é necessário executar `unlock` separadamente.
- `erase` executa Force Erase, destrói permanentemente todo o conteúdo e
  remove a senha.
- Operações que alteram a proteção ou destroem dados são recusadas quando o
  cartão ou alguma de suas partições está montado. A imagem Lite dedicada não
  monta automaticamente o cartão inserido no leitor nativo.
- Locks transitórios causados pelo `udev` são aguardados por até 60 segundos.
  A ferramenta não interrompe o serviço `udev` e não encerra processos.

## Fluxos recomendados

Recuperar os dados quando a senha é conhecida e remover a proteção de forma
permanente:

```bash
sudo sd-cmd42 clear /dev/mmcblk0
```

O comando solicita a senha sem eco, desbloqueia o cartão quando necessário,
remove a senha e carrega a tabela de partições somente ao final. Isso evita a
corrida observada entre `unlock`, a sondagem automática do `udev` e um
`clear` executado logo em seguida.

Recuperar somente o cartão, descartando permanentemente os dados, quando a
senha não é conhecida:

```bash
sudo sd-cmd42 erase /dev/mmcblk0 --confirm-erase
```

Confira cuidadosamente o dispositivo e a capacidade exibidos antes de
confirmar o Force Erase. Ao terminar, remova e reinsira o cartão; ele ficará
sem senha, sem tabela de partições e precisará ser particionado e formatado.

## Validação prática

O ciclo completo abaixo foi validado em uma Raspberry Pi 4 com o leitor SD
nativo e um cartão SDXC de 128 GB:

1. `set`
2. `lock`
3. `unlock`
4. `clear`, incluindo solicitação de senha sem eco
5. remoção e reinserção, confirmando que a senha foi eliminada
6. `set-lock`
7. `erase`, confirmando a remoção da senha e da tabela de partições

O kernel resultante deve ser instalado com um caminho de rollback; não
sobrescreva o kernel funcional sem backup.

## Imagem de recuperação

O workflow **Build Raspberry Pi CMD42 rescue image** usa a branch ARM64 do
projeto oficial `RPi-Distro/pi-gen`, fixada em um commit conhecido, e gera:

```text
raspberrypi-cmd42-rescue-*.img.xz
SHA256SUMS
```

A imagem não contém senha padrão, credenciais Wi-Fi, chaves SSH pessoais nem
as senhas dos cartões testados. No primeiro boot, conectado a monitor e
teclado, o assistente do Raspberry Pi OS solicita a criação do usuário e da
senha. Depois disso, use:

```bash
sudo cmd42-rescue
```

A Raspberry Pi 4 precisa iniciar essa imagem por um dispositivo USB, pois o
leitor microSD integrado deve permanecer disponível para o cartão alvo. O
`BOOT_ORDER` recomendado no EEPROM é `0xf14`, que tenta USB antes de SD.

Para gerar a imagem no GitHub:

1. abra **Actions**;
2. selecione **Build Raspberry Pi CMD42 rescue image**;
3. escolha **Run workflow** na branch `cmd42-kernel`;
4. baixe o artefato `raspberry-pi4-cmd42-rescue-image` ao término;
5. valide o arquivo com `SHA256SUMS` antes de gravá-lo.
