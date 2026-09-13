/*
 * sd-cmd42.c - Minimal userspace helper for SD CMD42 unlock, password clearing and force erase.
 *
 * GPL-2.0-only. Force Erase permanently destroys all card data and clears
 * the password. Operations are restricted to whole native MMC block devices
 * whose sysfs type is "SD".
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

#define MMC_SEND_STATUS   13U
#define MMC_SET_BLOCKLEN  16U
#define MMC_LOCK_UNLOCK   42U

#define MMC_CMD42_UNLOCK        0x00U
#define MMC_CMD42_CLEAR_PASSWORD 0x02U
#define MMC_CMD42_FORCE_ERASE   0x08U
#define SD_MAX_PASSWORD_LEN   16U

#define R1_CARD_IS_LOCKED     (1U << 25)
#define R1_LOCK_UNLOCK_FAILED (1U << 24)
#define R1_ILLEGAL_COMMAND    (1U << 22)
#define R1_ERROR              (1U << 19)

#define CMD42_UNLOCK_TIMEOUT_MS 60000U
#define CMD42_ERASE_TIMEOUT_MS  600000U

static void usage(const char *program)
{
	fprintf(stderr,
		"Uso:\n"
		"  %s unlock /dev/mmcblkN SENHA\n"
		"  %s clear  /dev/mmcblkN SENHA\n"
		"  %s erase  /dev/mmcblkN --confirm-erase\n\n"
		"unlock preserva os dados e dura ate o cartao perder alimentacao.\n"
		"clear remove a senha permanentemente e preserva todos os dados.\n"
		"erase apaga permanentemente todo o cartao e remove a senha.\n",
		program, program, program);
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

static int read_rca(const char *base, uint32_t *rca)
{
	char path[PATH_MAX];
	char value[32] = {0};
	char *end;
	unsigned long parsed;
	FILE *file;

	if (snprintf(path, sizeof(path), "/sys/class/block/%s/device/rca", base)
	    >= (int)sizeof(path))
		return -1;

	file = fopen(path, "r");
	if (!file) {
		fprintf(stderr, "Nao foi possivel abrir %s: %s\n",
			path, strerror(errno));
		return -1;
	}
	if (!fgets(value, sizeof(value), file)) {
		fprintf(stderr, "Nao foi possivel ler %s.\n", path);
		fclose(file);
		return -1;
	}
	fclose(file);

	errno = 0;
	parsed = strtoul(value, &end, 16);
	if (errno || parsed > 0xffffUL || end == value) {
		fprintf(stderr, "RCA invalido em %s: %s\n", path, value);
		return -1;
	}
	*rca = (uint32_t)parsed;
	return 0;
}

static int validate_target(const char *device, char *resolved,
			   size_t resolved_size, char *base, size_t base_size)
{
	const char *name;
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

	name = strrchr(resolved, '/');
	name = name ? name + 1 : resolved;
	if (strlen(name) + 1 > base_size)
		return -1;
	strcpy(base, name);

	if (strncmp(base, "mmcblk", 6) != 0 || base[6] == '\0') {
		fprintf(stderr, "Dispositivo recusado: use um disco inteiro /dev/mmcblkN.\n");
		return -1;
	}

	errno = 0;
	index = strtol(base + 6, &end, 10);
	if (errno || index < 0 || *end != '\0') {
		fprintf(stderr, "Dispositivo recusado: particoes nao sao aceitas.\n");
		return -1;
	}

	if (stat(resolved, &st) != 0 || !S_ISBLK(st.st_mode)) {
		fprintf(stderr, "%s nao e um dispositivo de bloco valido.\n", resolved);
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
	return 0;
}

static int send_status(int fd, uint32_t rca, uint32_t *status)
{
	struct mmc_ioc_cmd command = {0};

	command.opcode = MMC_SEND_STATUS;
	command.arg = rca << 16;
	command.flags = MMC_RSP_R1 | MMC_CMD_AC;
	command.cmd_timeout_ms = 10000;

	if (ioctl(fd, MMC_IOC_CMD, &command) < 0) {
		fprintf(stderr, "CMD13 falhou: %s\n", strerror(errno));
		return -1;
	}
	*status = command.response[0];
	return 0;
}

static int send_cmd42(int fd, uint8_t *payload, uint32_t length,
		      uint32_t timeout_ms, uint32_t *response)
{
	struct mmc_ioc_cmd command = {0};

	if (set_block_length(fd, length) != 0)
		return -1;

	command.write_flag = 1;
	command.opcode = MMC_LOCK_UNLOCK;
	command.arg = 0;
	command.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_ADTC;
	command.blksz = length;
	command.blocks = 1;
	command.cmd_timeout_ms = timeout_ms;
	mmc_ioc_cmd_set_data(command, payload);

	if (ioctl(fd, MMC_IOC_CMD, &command) < 0) {
		fprintf(stderr, "CMD42 falhou: %s\n", strerror(errno));
		return -1;
	}
	*response = command.response[0];
	return 0;
}

static int unlock_card(int fd, const char *password, uint32_t rca,
		       uint32_t *status)
{
	size_t password_length = strlen(password);
	uint8_t payload[2 + SD_MAX_PASSWORD_LEN] = {0};
	uint32_t command_response = 0;
	uint8_t sector[512];

	if (password_length == 0 || password_length > SD_MAX_PASSWORD_LEN) {
		fprintf(stderr, "A senha SD deve possuir de 1 a %u bytes.\n",
			SD_MAX_PASSWORD_LEN);
		return -1;
	}

	payload[0] = MMC_CMD42_UNLOCK;
	payload[1] = (uint8_t)password_length;
	memcpy(payload + 2, password, password_length);

	if (send_cmd42(fd, payload, (uint32_t)(2 + password_length),
		       CMD42_UNLOCK_TIMEOUT_MS, &command_response) != 0)
		return -1;

	usleep(100000);
	if (send_status(fd, rca, status) != 0)
		return -1;

	if (*status & (R1_LOCK_UNLOCK_FAILED | R1_ILLEGAL_COMMAND | R1_ERROR)) {
		fprintf(stderr, "Desbloqueio recusado (R1=0x%08x).\n", *status);
		return -1;
	}
	if (*status & R1_CARD_IS_LOCKED) {
		fprintf(stderr, "Senha incorreta ou cartao ainda bloqueado (R1=0x%08x).\n",
			*status);
		return -1;
	}

	if (pread(fd, sector, sizeof(sector), 0) != (ssize_t)sizeof(sector)) {
		fprintf(stderr, "Desbloqueou, mas a leitura do setor 0 falhou: %s\n",
			strerror(errno));
		return -1;
	}

	if (ioctl(fd, BLKRRPART) != 0)
		fprintf(stderr, "Aviso: releitura da tabela de particoes falhou: %s\n",
			strerror(errno));

	return 0;
}

static int clear_password(int fd, const char *password, uint32_t rca,
			  uint32_t *status)
{
	size_t password_length = strlen(password);
	uint8_t payload[2 + SD_MAX_PASSWORD_LEN] = {0};
	uint32_t command_response = 0;

	if (password_length == 0 || password_length > SD_MAX_PASSWORD_LEN) {
		fprintf(stderr, "A senha SD deve possuir de 1 a %u bytes.\n",
			SD_MAX_PASSWORD_LEN);
		return -1;
	}

	payload[0] = MMC_CMD42_CLEAR_PASSWORD;
	payload[1] = (uint8_t)password_length;
	memcpy(payload + 2, password, password_length);

	if (send_cmd42(fd, payload, (uint32_t)(2 + password_length),
		       CMD42_UNLOCK_TIMEOUT_MS, &command_response) != 0)
		return -1;

	usleep(100000);
	if (send_status(fd, rca, status) != 0)
		return -1;

	if (*status & (R1_LOCK_UNLOCK_FAILED | R1_ILLEGAL_COMMAND | R1_ERROR)) {
		fprintf(stderr, "Remocao da senha recusada (R1=0x%08x).\n", *status);
		return -1;
	}
	if (*status & R1_CARD_IS_LOCKED) {
		fprintf(stderr, "O cartao permaneceu bloqueado (R1=0x%08x).\n",
			*status);
		return -1;
	}

	return 0;
}

static int force_erase(int fd, uint32_t *response)
{
	uint8_t payload[2] = { MMC_CMD42_FORCE_ERASE, 0x00 };

	if (send_cmd42(fd, payload, sizeof(payload), CMD42_ERASE_TIMEOUT_MS,
		       response) != 0)
		return -1;
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
	char base[NAME_MAX];
	uint64_t size = 0;
	uint32_t rca = 0;
	uint32_t response = 0;
	int fd;
	int unlock_mode;
	int clear_mode;
	int erase_mode;

	unlock_mode = argc == 4 && strcmp(argv[1], "unlock") == 0;
	clear_mode = argc == 4 && strcmp(argv[1], "clear") == 0;
	erase_mode = argc == 4 && strcmp(argv[1], "erase") == 0 &&
		     strcmp(argv[3], "--confirm-erase") == 0;
	if (!unlock_mode && !clear_mode && !erase_mode) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (validate_target(argv[2], resolved, sizeof(resolved),
			    base, sizeof(base)) != 0)
		return EXIT_FAILURE;
	if (read_rca(base, &rca) != 0)
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

	fprintf(stderr, "ALVO VALIDADO: %s, tipo SD, %.1f GiB, RCA=0x%04x.\n",
		resolved, (double)size / (1024.0 * 1024.0 * 1024.0), rca);

	if (unlock_mode) {
		fprintf(stderr, "Enviando CMD42 Unlock...\n");
		if (unlock_card(fd, argv[3], rca, &response) != 0) {
			close(fd);
			return EXIT_FAILURE;
		}
		fprintf(stderr,
			"SUCESSO: senha aceita; cartao desbloqueado (R1=0x%08x).\n",
			response);
	} else if (clear_mode) {
		fprintf(stderr, "Enviando CMD42 Clear Password...\n");
		if (clear_password(fd, argv[3], rca, &response) != 0) {
			close(fd);
			return EXIT_FAILURE;
		}
		fprintf(stderr,
			"SUCESSO: senha removida sem apagar os dados (R1=0x%08x).\n",
			response);
	} else {
		fprintf(stderr,
			"Enviando CMD42 Force Erase; nao desligue a alimentacao...\n");
		if (force_erase(fd, &response) != 0) {
			close(fd);
			return EXIT_FAILURE;
		}
		fprintf(stderr,
			"CMD42 concluido (R1 inicial=0x%08x).\n"
			"Remova e reinsira o cartao para reinicializa-lo.\n",
			response);
		fsync(fd);
	}

	close(fd);
	return EXIT_SUCCESS;
}
