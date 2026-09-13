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
#include <termios.h>
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

#define MMC_CMD42_UNLOCK         0x00U
#define MMC_CMD42_SET_PASSWORD   0x01U
#define MMC_CMD42_CLEAR_PASSWORD 0x02U
#define MMC_CMD42_LOCK           0x04U
#define MMC_CMD42_SET_AND_LOCK   0x05U
#define MMC_CMD42_FORCE_ERASE    0x08U
#define SD_MAX_PASSWORD_LEN      16U
#define CMD42_PASSWORD_BLOCK_SIZE 512U

#define R1_CARD_IS_LOCKED     (1U << 25)
#define R1_LOCK_UNLOCK_FAILED (1U << 24)
#define R1_ILLEGAL_COMMAND    (1U << 22)
#define R1_ERROR              (1U << 19)

#define CMD42_PASSWORD_TIMEOUT_MS 60000U
#define CMD42_ERASE_TIMEOUT_MS   600000U
#define EXCLUSIVE_LOCK_TIMEOUT_MS  60000U
#define EXCLUSIVE_LOCK_RETRY_MS      250U

enum operation {
	OP_NONE,
	OP_UNLOCK,
	OP_CLEAR,
	OP_SET,
	OP_LOCK,
	OP_SET_LOCK,
	OP_ERASE,
};

static void usage(const char *program)
{
	fprintf(stderr,
		"Uso:\n"
		"  %s unlock   /dev/mmcblkN [SENHA]\n"
		"  %s clear    /dev/mmcblkN [SENHA]\n"
		"  %s set      /dev/mmcblkN [NOVA_SENHA]\n"
		"  %s lock     /dev/mmcblkN [SENHA] --confirm-lock\n"
		"  %s set-lock /dev/mmcblkN [NOVA_SENHA] --confirm-lock\n"
		"  %s erase    /dev/mmcblkN --confirm-erase\n\n"
		"Sem SENHA na linha de comando, a ferramenta solicita-a sem eco.\n"
		"set grava uma senha persistente sem bloquear a sessao atual.\n"
		"lock bloqueia imediatamente usando a senha configurada.\n"
		"set-lock grava uma senha e bloqueia imediatamente.\n"
		"unlock preserva os dados e dura ate o cartao perder alimentacao.\n"
		"clear remove a senha permanentemente e preserva todos os dados.\n"
		"erase apaga permanentemente todo o cartao e remove a senha.\n",
		program, program, program, program, program, program);
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

static int validate_password(const char *password)
{
	size_t length = strlen(password);
	size_t i;

	if (length == 0 || length > SD_MAX_PASSWORD_LEN) {
		fprintf(stderr, "A senha SD deve possuir de 1 a %u bytes.\n",
			SD_MAX_PASSWORD_LEN);
		return -1;
	}
	for (i = 0; i < length; ++i) {
		unsigned char character = (unsigned char)password[i];

		if (character < 0x20 || character > 0x7e) {
			fprintf(stderr,
				"Use somente caracteres ASCII imprimiveis na senha.\n");
			return -1;
		}
	}
	return 0;
}

static int read_password(const char *prompt, char *password, size_t size)
{
	struct termios original;
	struct termios hidden;
	FILE *tty;
	int descriptor;

	tty = fopen("/dev/tty", "r+");
	if (!tty) {
		fprintf(stderr, "Nao foi possivel abrir /dev/tty: %s\n",
			strerror(errno));
		return -1;
	}
	descriptor = fileno(tty);
	if (tcgetattr(descriptor, &original) != 0) {
		fprintf(stderr, "tcgetattr falhou: %s\n", strerror(errno));
		fclose(tty);
		return -1;
	}
	hidden = original;
	hidden.c_lflag &= (tcflag_t)~ECHO;

	fputs(prompt, tty);
	fflush(tty);
	if (tcsetattr(descriptor, TCSAFLUSH, &hidden) != 0) {
		fprintf(stderr, "tcsetattr falhou: %s\n", strerror(errno));
		fclose(tty);
		return -1;
	}
	if (!fgets(password, (int)size, tty)) {
		(void)tcsetattr(descriptor, TCSAFLUSH, &original);
		fputc('\n', tty);
		fprintf(stderr, "Nao foi possivel ler a senha.\n");
		fclose(tty);
		return -1;
	}
	(void)tcsetattr(descriptor, TCSAFLUSH, &original);
	fputc('\n', tty);
	fclose(tty);
	password[strcspn(password, "\r\n")] = '\0';
	return validate_password(password);
}

static int read_new_password(char *password, size_t size)
{
	char confirmation[SD_MAX_PASSWORD_LEN + 2] = {0};
	int result;

	if (read_password("Nova senha (1-16 caracteres ASCII): ",
			  password, size) != 0)
		return -1;
	if (read_password("Confirme a nova senha: ", confirmation,
			  sizeof(confirmation)) != 0) {
		explicit_bzero(password, size);
		return -1;
	}
	result = strcmp(password, confirmation);
	explicit_bzero(confirmation, sizeof(confirmation));
	if (result != 0) {
		fprintf(stderr, "As senhas nao conferem. Nenhum comando foi enviado.\n");
		explicit_bzero(password, size);
		return -1;
	}
	return 0;
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

static int refresh_partitions(int fd)
{
	uint8_t sector[512];

	if (pread(fd, sector, sizeof(sector), 0) != (ssize_t)sizeof(sector)) {
		fprintf(stderr, "A leitura do setor 0 falhou: %s\n",
			strerror(errno));
		return -1;
	}

	if (ioctl(fd, BLKRRPART) != 0)
		fprintf(stderr, "Aviso: releitura da tabela de particoes falhou: %s\n",
			strerror(errno));

	return 0;
}

static int unlock_card(int fd, const char *password, uint32_t rca,
		       uint32_t *status, int refresh)
{
	size_t password_length = strlen(password);
	uint8_t payload[CMD42_PASSWORD_BLOCK_SIZE] = {0};
	uint32_t command_response = 0;

	if (password_length == 0 || password_length > SD_MAX_PASSWORD_LEN) {
		fprintf(stderr, "A senha SD deve possuir de 1 a %u bytes.\n",
			SD_MAX_PASSWORD_LEN);
		return -1;
	}

	payload[0] = MMC_CMD42_UNLOCK;
	payload[1] = (uint8_t)password_length;
	memcpy(payload + 2, password, password_length);

	if (send_cmd42(fd, payload, sizeof(payload),
		       CMD42_PASSWORD_TIMEOUT_MS, &command_response) != 0)
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

	return refresh ? refresh_partitions(fd) : 0;
}

static int clear_password(int fd, const char *password, uint32_t rca,
			  uint32_t *status)
{
	size_t password_length = strlen(password);
	uint8_t payload[CMD42_PASSWORD_BLOCK_SIZE] = {0};
	uint32_t command_response = 0;

	if (password_length == 0 || password_length > SD_MAX_PASSWORD_LEN) {
		fprintf(stderr, "A senha SD deve possuir de 1 a %u bytes.\n",
			SD_MAX_PASSWORD_LEN);
		return -1;
	}

	payload[0] = MMC_CMD42_CLEAR_PASSWORD;
	payload[1] = (uint8_t)password_length;
	memcpy(payload + 2, password, password_length);

	if (send_cmd42(fd, payload, sizeof(payload),
		       CMD42_PASSWORD_TIMEOUT_MS, &command_response) != 0)
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

static int set_password(int fd, const char *password, uint32_t rca,
			uint32_t *status, int lock_now)
{
	size_t password_length = strlen(password);
	uint8_t payload[CMD42_PASSWORD_BLOCK_SIZE] = {0};
	uint32_t command_response = 0;

	if (validate_password(password) != 0)
		return -1;
	payload[0] = lock_now ? MMC_CMD42_SET_AND_LOCK :
					 MMC_CMD42_SET_PASSWORD;
	payload[1] = (uint8_t)password_length;
	memcpy(payload + 2, password, password_length);

	if (send_cmd42(fd, payload, sizeof(payload),
		       CMD42_PASSWORD_TIMEOUT_MS, &command_response) != 0)
		return -1;
	usleep(100000);
	if (send_status(fd, rca, status) != 0)
		return -1;
	if (*status & (R1_LOCK_UNLOCK_FAILED | R1_ILLEGAL_COMMAND | R1_ERROR)) {
		fprintf(stderr, "Gravacao da senha recusada (R1=0x%08x).\n",
			*status);
		return -1;
	}
	if (lock_now && !(*status & R1_CARD_IS_LOCKED)) {
		fprintf(stderr, "O cartao nao confirmou o bloqueio (R1=0x%08x).\n",
			*status);
		return -1;
	}
	if (!lock_now && (*status & R1_CARD_IS_LOCKED)) {
		fprintf(stderr,
			"A senha foi gravada, mas o cartao bloqueou inesperadamente.\n");
		return -1;
	}
	return 0;
}

static int lock_card(int fd, const char *password, uint32_t rca,
		     uint32_t *status)
{
	size_t password_length = strlen(password);
	uint8_t payload[CMD42_PASSWORD_BLOCK_SIZE] = {0};
	uint32_t command_response = 0;

	if (validate_password(password) != 0)
		return -1;
	payload[0] = MMC_CMD42_LOCK;
	payload[1] = (uint8_t)password_length;
	memcpy(payload + 2, password, password_length);

	if (send_cmd42(fd, payload, sizeof(payload),
		       CMD42_PASSWORD_TIMEOUT_MS, &command_response) != 0)
		return -1;
	usleep(100000);
	if (send_status(fd, rca, status) != 0)
		return -1;
	if (*status & (R1_LOCK_UNLOCK_FAILED | R1_ILLEGAL_COMMAND | R1_ERROR)) {
		fprintf(stderr, "Bloqueio recusado (R1=0x%08x).\n", *status);
		return -1;
	}
	if (!(*status & R1_CARD_IS_LOCKED)) {
		fprintf(stderr, "O cartao nao confirmou o bloqueio (R1=0x%08x).\n",
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

static int target_has_mounts(const char *base)
{
	char line[4096];
	char needle[NAME_MAX + 8];
	FILE *file;

	if (snprintf(needle, sizeof(needle), "/dev/%s", base) >=
	    (int)sizeof(needle))
		return -1;
	file = fopen("/proc/self/mountinfo", "r");
	if (!file) {
		fprintf(stderr, "Nao foi possivel verificar os mounts: %s\n",
			strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), file)) {
		char *separator = strstr(line, " - ");
		char *match = separator ? strstr(separator, needle) : NULL;
		char following;

		if (!match)
			continue;
		following = match[strlen(needle)];
		if (following == 'p' || following == ' ' || following == '\n' ||
		    following == '\0') {
			fprintf(stderr,
				"Operacao recusada: /dev/%s ou uma de suas particoes esta montada.\n",
				base);
			fclose(file);
			return 1;
		}
	}
	fclose(file);
	return 0;
}

static int acquire_exclusive_lock(int fd, const char *device)
{
	unsigned int waited_ms = 0;
	int announced = 0;

	for (;;) {
		if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
			if (announced)
				fprintf(stderr, "Dispositivo liberado; continuando.\n");
			return 0;
		}

		if (errno != EWOULDBLOCK && errno != EAGAIN) {
			fprintf(stderr, "Nao foi possivel bloquear exclusivamente %s: %s\n",
				device, strerror(errno));
			return -1;
		}

		if (!announced) {
			fprintf(stderr,
				"%s esta temporariamente ocupado; aguardando ate %u segundos...\n",
				device, EXCLUSIVE_LOCK_TIMEOUT_MS / 1000U);
			announced = 1;
		}

		if (waited_ms >= EXCLUSIVE_LOCK_TIMEOUT_MS) {
			fprintf(stderr,
				"Tempo esgotado: %s continua em uso. Verifique mounts e processos.\n",
				device);
			return -1;
		}

		usleep(EXCLUSIVE_LOCK_RETRY_MS * 1000U);
		waited_ms += EXCLUSIVE_LOCK_RETRY_MS;
	}
}

static enum operation parse_operation(const char *name)
{
	if (strcmp(name, "unlock") == 0)
		return OP_UNLOCK;
	if (strcmp(name, "clear") == 0)
		return OP_CLEAR;
	if (strcmp(name, "set") == 0)
		return OP_SET;
	if (strcmp(name, "lock") == 0)
		return OP_LOCK;
	if (strcmp(name, "set-lock") == 0)
		return OP_SET_LOCK;
	if (strcmp(name, "erase") == 0)
		return OP_ERASE;
	return OP_NONE;
}

int main(int argc, char **argv)
{
	char resolved[PATH_MAX];
	char base[NAME_MAX];
	char password[SD_MAX_PASSWORD_LEN + 2] = {0};
	uint64_t size = 0;
	uint32_t rca = 0;
	uint32_t response = 0;
	enum operation operation;
	const char *supplied_password = NULL;
	int fd;

	if (argc < 3) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	operation = parse_operation(argv[1]);
	if (operation == OP_NONE) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if ((operation == OP_UNLOCK || operation == OP_CLEAR ||
	     operation == OP_SET) && argc == 4)
		supplied_password = argv[3];
	else if ((operation == OP_UNLOCK || operation == OP_CLEAR ||
		  operation == OP_SET) && argc != 3) {
		usage(argv[0]);
		return EXIT_FAILURE;
	} else if (operation == OP_LOCK || operation == OP_SET_LOCK) {
		if (argc == 5 && strcmp(argv[4], "--confirm-lock") == 0)
			supplied_password = argv[3];
		else if (argc != 4 || strcmp(argv[3], "--confirm-lock") != 0) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	} else if (operation == OP_ERASE &&
		   (argc != 4 || strcmp(argv[3], "--confirm-erase") != 0)) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (validate_target(argv[2], resolved, sizeof(resolved),
			    base, sizeof(base)) != 0)
		return EXIT_FAILURE;
	if (read_rca(base, &rca) != 0)
		return EXIT_FAILURE;
	if (operation == OP_CLEAR || operation == OP_SET || operation == OP_LOCK ||
	    operation == OP_SET_LOCK || operation == OP_ERASE) {
		if (target_has_mounts(base) != 0)
			return EXIT_FAILURE;
	}

	fd = open(resolved, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", resolved, strerror(errno));
		return EXIT_FAILURE;
	}
	if (acquire_exclusive_lock(fd, resolved) != 0) {
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

	if ((operation == OP_SET || operation == OP_SET_LOCK) &&
	    !supplied_password) {
		if (read_new_password(password, sizeof(password)) != 0) {
			close(fd);
			return EXIT_FAILURE;
		}
	} else if (operation != OP_ERASE) {
		if (supplied_password) {
			fprintf(stderr,
				"AVISO: a senha fornecida como argumento pode ficar no historico.\n");
			if (validate_password(supplied_password) != 0) {
				close(fd);
				return EXIT_FAILURE;
			}
			strcpy(password, supplied_password);
		} else if (read_password("Senha: ", password,
					 sizeof(password)) != 0) {
			close(fd);
			return EXIT_FAILURE;
		}
	}

	if (operation == OP_SET || operation == OP_LOCK ||
	    operation == OP_SET_LOCK || operation == OP_ERASE) {
		sync();
		if (ioctl(fd, BLKFLSBUF) != 0)
			fprintf(stderr, "Aviso: BLKFLSBUF falhou: %s\n", strerror(errno));
	}

	if (operation == OP_UNLOCK) {
		fprintf(stderr, "Enviando CMD42 Unlock...\n");
		if (unlock_card(fd, password, rca, &response, 1) != 0) {
			explicit_bzero(password, sizeof(password));
			close(fd);
			return EXIT_FAILURE;
		}
		fprintf(stderr,
			"SUCESSO: senha aceita; cartao desbloqueado (R1=0x%08x).\n",
			response);
	} else if (operation == OP_CLEAR) {
		uint32_t initial_status = 0;
		int was_locked;

		if (send_status(fd, rca, &initial_status) != 0) {
			explicit_bzero(password, sizeof(password));
			close(fd);
			return EXIT_FAILURE;
		}
		was_locked = !!(initial_status & R1_CARD_IS_LOCKED);
		if (was_locked) {
			fprintf(stderr,
				"Cartao bloqueado; desbloqueando antes de remover a senha...\n");
			if (unlock_card(fd, password, rca, &response, 0) != 0) {
				explicit_bzero(password, sizeof(password));
				close(fd);
				return EXIT_FAILURE;
			}
		}
		fprintf(stderr, "Enviando CMD42 Clear Password...\n");
		if (clear_password(fd, password, rca, &response) != 0) {
			explicit_bzero(password, sizeof(password));
			close(fd);
			return EXIT_FAILURE;
		}
		fprintf(stderr,
			"SUCESSO: senha removida sem apagar os dados (R1=0x%08x).\n",
			response);
		if (was_locked && refresh_partitions(fd) != 0)
			fprintf(stderr,
				"Aviso: reinsira o cartao para carregar suas particoes.\n");
	} else if (operation == OP_SET) {
		fprintf(stderr, "Enviando CMD42 Set Password...\n");
		if (set_password(fd, password, rca, &response, 0) != 0) {
			explicit_bzero(password, sizeof(password));
			close(fd);
			return EXIT_FAILURE;
		}
		fprintf(stderr,
			"SUCESSO: senha gravada; sessao atual permanece desbloqueada "
			"(R1=0x%08x).\n",
			response);
	} else if (operation == OP_LOCK) {
		fprintf(stderr, "Enviando CMD42 Lock...\n");
		if (lock_card(fd, password, rca, &response) != 0) {
			explicit_bzero(password, sizeof(password));
			close(fd);
			return EXIT_FAILURE;
		}
		fprintf(stderr, "SUCESSO: cartao bloqueado (R1=0x%08x).\n",
			response);
	} else if (operation == OP_SET_LOCK) {
		fprintf(stderr, "Enviando CMD42 Set Password + Lock...\n");
		if (set_password(fd, password, rca, &response, 1) != 0) {
			explicit_bzero(password, sizeof(password));
			close(fd);
			return EXIT_FAILURE;
		}
		fprintf(stderr,
			"SUCESSO: senha gravada e cartao bloqueado (R1=0x%08x).\n",
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

	explicit_bzero(password, sizeof(password));
	close(fd);
	return EXIT_SUCCESS;
}
