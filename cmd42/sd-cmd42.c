/*
 * sd-cmd42.c - Minimal userspace helper for SD CMD42 force erase.
 *
 * GPL-2.0-only. This command permanently destroys all card data and clears
 * the card password. It is intentionally restricted to whole native MMC
 * block devices whose sysfs type is "SD".
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <linux/mmc/ioctl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define MMC_RSP_PRESENT  (1U << 0)
#define MMC_RSP_CRC      (1U << 2)
#define MMC_RSP_BUSY     (1U << 3)
#define MMC_RSP_OPCODE   (1U << 4)
#define MMC_CMD_AC       (0U << 5)
#define MMC_CMD_ADTC     (1U << 5)
#define MMC_RSP_SPI_R1   (1U << 7)
#define MMC_RSP_SPI_R1B  (1U << 7)

#define MMC_RSP_R1  (MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)
#define MMC_RSP_R1B (MMC_RSP_R1 | MMC_RSP_BUSY)

#define MMC_SET_BLOCKLEN 16U
#define MMC_LOCK_UNLOCK  42U
#define MMC_CMD42_FORCE_ERASE 0x08U

#define R1_CARD_IS_LOCKED     (1U << 25)
#define R1_LOCK_UNLOCK_FAILED (1U << 24)
#define CMD42_TIMEOUT_MS      600000U

static void usage(const char *program)
{
	fprintf(stderr,
		"Uso:\n"
		"  %s erase /dev/mmcblkN --confirm-erase\n\n"
		"ATENCAO: erase apaga permanentemente todo o cartao e remove a senha.\n",
		program);
}

static int read_sd_type(const char *base)
{
	char path[PATH_MAX];
	char type[32] = {0};
	FILE *file;

	if (snprintf(path, sizeof(path), "/sys/class/block/%s/device/type", base)
	    >= (int)sizeof(path)) {
		fprintf(stderr, "Caminho sysfs muito longo.\n");
		return -1;
	}

	file = fopen(path, "r");
	if (!file) {
		fprintf(stderr, "Nao foi possivel abrir %s: %s\n",
			path, strerror(errno));
		return -1;
	}

	if (!fgets(type, sizeof(type), file)) {
		fprintf(stderr, "Nao foi possivel ler %s.\n", path);
		fclose(file);
		return -1;
	}
	fclose(file);
	type[strcspn(type, "\r\n")] = '\0';

	if (strcmp(type, "SD") != 0) {
		fprintf(stderr, "Dispositivo recusado: tipo sysfs '%s', esperado 'SD'.\n",
			type);
		return -1;
	}

	return 0;
}

static int validate_target(const char *device, char *resolved, size_t resolved_size)
{
	const char *base;
	struct stat st;
	char *end;
	long index;

	if (!realpath(device, resolved)) {
		fprintf(stderr, "realpath(%s): %s\n", device, strerror(errno));
		return -1;
	}
	if (strlen(resolved) + 1 > resolved_size) {
		fprintf(stderr, "Caminho do dispositivo muito longo.\n");
		return -1;
	}

	base = strrchr(resolved, '/');
	base = base ? base + 1 : resolved;

	if (strncmp(base, "mmcblk", 6) != 0 || base[6] == '\0') {
		fprintf(stderr, "Dispositivo recusado: use um disco inteiro /dev/mmcblkN.\n");
		return -1;
	}

	errno = 0;
	index = strtol(base + 6, &end, 10);
	if (errno || index < 0 || *end != '\0') {
		fprintf(stderr, "Dispositivo recusado: particoes e nomes invalidos nao sao aceitos.\n");
		return -1;
	}

	if (stat(resolved, &st) != 0) {
		fprintf(stderr, "stat(%s): %s\n", resolved, strerror(errno));
		return -1;
	}
	if (!S_ISBLK(st.st_mode)) {
		fprintf(stderr, "%s nao e um dispositivo de bloco.\n", resolved);
		return -1;
	}

	return read_sd_type(base);
}

static int set_block_length(int fd, uint32_t length)
{
	struct mmc_ioc_cmd command = {0};

	command.opcode = MMC_SET_BLOCKLEN;
	command.arg = length;
	command.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;
	command.cmd_timeout_ms = 10000;

	if (ioctl(fd, MMC_IOC_CMD, &command) < 0) {
		fprintf(stderr, "CMD16 (%" PRIu32 " bytes) falhou: %s\n",
			length, strerror(errno));
		return -1;
	}

	if (command.response[0] & R1_LOCK_UNLOCK_FAILED) {
		fprintf(stderr, "CMD16 retornou erro R1: 0x%08x\n",
			command.response[0]);
		return -1;
	}

	return 0;
}

static int force_erase(int fd, uint32_t *response)
{
	uint8_t payload[2] = { MMC_CMD42_FORCE_ERASE, 0x00 };
	struct mmc_ioc_cmd command = {0};

	if (set_block_length(fd, sizeof(payload)) != 0)
		return -1;

	command.write_flag = 1;
	command.opcode = MMC_LOCK_UNLOCK;
	command.arg = 0;
	command.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_ADTC;
	command.blksz = sizeof(payload);
	command.blocks = 1;
	command.cmd_timeout_ms = CMD42_TIMEOUT_MS;
	mmc_ioc_cmd_set_data(command, payload);

	if (ioctl(fd, MMC_IOC_CMD, &command) < 0) {
		fprintf(stderr, "CMD42 Force Erase falhou: %s\n", strerror(errno));
		return -1;
	}

	*response = command.response[0];
	if (*response & R1_LOCK_UNLOCK_FAILED) {
		fprintf(stderr, "O cartao recusou o Force Erase (R1=0x%08x).\n",
			*response);
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	char resolved[PATH_MAX];
	uint64_t size = 0;
	uint32_t response = 0;
	int fd;

	if (argc != 4 || strcmp(argv[1], "erase") != 0 ||
	    strcmp(argv[3], "--confirm-erase") != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (validate_target(argv[2], resolved, sizeof(resolved)) != 0)
		return EXIT_FAILURE;

	fd = open(resolved, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", resolved, strerror(errno));
		return EXIT_FAILURE;
	}

	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		fprintf(stderr, "Nao foi possivel bloquear exclusivamente %s: %s\n",
			resolved, strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	if (ioctl(fd, BLKGETSIZE64, &size) != 0) {
		fprintf(stderr, "BLKGETSIZE64 falhou: %s\n", strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	fprintf(stderr,
		"ALVO VALIDADO: %s, tipo SD, %.1f GiB.\n"
		"Enviando CMD42 Force Erase; nao desligue a alimentacao...\n",
		resolved, (double)size / (1024.0 * 1024.0 * 1024.0));

	if (force_erase(fd, &response) != 0) {
		close(fd);
		return EXIT_FAILURE;
	}

	fprintf(stderr,
		"CMD42 concluido (R1=0x%08x, bloqueado=%s).\n"
		"Remova e reinsira o cartao para reinicializa-lo.\n",
		response,
		(response & R1_CARD_IS_LOCKED) ? "sim" : "nao");

	fsync(fd);
	close(fd);
	return EXIT_SUCCESS;
}
