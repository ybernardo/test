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
- `build-kernel.sh`: baixa os pacotes oficiais exatos da Raspberry Pi,
  recupera a configuração do kernel oficial e faz cross-compilation ARM64.
- `.github/workflows/build-cmd42-kernel.yml`: executa o build no GitHub
  Actions e publica o `Image.gz`, a configuração, o patch e checksums.

## Compilar o utilitário

```bash
gcc -O2 -Wall -Wextra -Werror -std=gnu11 \
  -o sd-cmd42 sd-cmd42.c
sudo install -m 0755 sd-cmd42 /usr/local/sbin/sd-cmd42
```

## Operações

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
- Operações que podem tornar os dados inacessíveis são recusadas quando o
  cartão ou alguma de suas partições está montado.
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
