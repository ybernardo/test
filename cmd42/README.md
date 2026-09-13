# Raspberry Pi 4 — kernel CMD42 para cartões SD bloqueados

Esta branch contém uma compilação reproduzível e experimental do kernel
`6.18.39+rpt-rpi-v8` usado no Raspberry Pi OS Debian 13 (Trixie).

O patch altera a inicialização do subsistema MMC para manter um cartão SD
protegido por senha registrado no sistema. Isso permite que um utilitário em
userspace envie o comando nativo SD `CMD42` pelo leitor microSD integrado da
Raspberry Pi 4.

## Conteúdo

- `patches/expose-locked-sd.patch`: alteração mínima em
  `drivers/mmc/core/sd.c`.
- `build-kernel.sh`: baixa os pacotes oficiais exatos da Raspberry Pi,
  recupera a configuração do kernel oficial e faz cross-compilation ARM64.
- `.github/workflows/build-cmd42-kernel.yml`: executa o build no GitHub
  Actions e publica o `Image.gz`, a configuração, o patch e checksums.

## Escopo e segurança

Este build apenas expõe o cartão bloqueado como dispositivo MMC. O envio de
`CMD42`, incluindo Force Erase, será feito posteriormente com uma ferramenta
separada. Force Erase destrói permanentemente todo o conteúdo do cartão.

O kernel resultante deve ser instalado com um caminho de rollback; não
sobrescreva o kernel funcional sem backup.
