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
sudo sd-cmd42 set      /dev/mmcblkN
sudo sd-cmd42 lock     /dev/mmcblkN --confirm-lock
sudo sd-cmd42 set-lock /dev/mmcblkN --confirm-lock
sudo sd-cmd42 unlock   /dev/mmcblkN
sudo sd-cmd42 clear    /dev/mmcblkN
sudo sd-cmd42 erase    /dev/mmcblkN --confirm-erase
```

As senhas são solicitadas sem eco. `unlock` e `clear` ainda aceitam uma
senha como argumento para compatibilidade, mas isso não é recomendado porque
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
- `clear` remove a senha permanentemente sem apagar os dados.
- `erase` executa Force Erase, destrói permanentemente todo o conteúdo e
  remove a senha.
- Operações que podem tornar os dados inacessíveis são recusadas quando o
  cartão ou alguma de suas partições está montado.

O kernel resultante deve ser instalado com um caminho de rollback; não
sobrescreva o kernel funcional sem backup.
