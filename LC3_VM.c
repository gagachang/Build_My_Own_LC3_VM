#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>

/*** Memory Mapped Registers ***/
enum
{
	MR_KBSR = 0xFE00,  /* keyboard status register */
	MR_KBDR = 0xFE02   /* keyboard data register */
};

/*** Trap Codes ***/
enum
{
	TRAP_GETC  = 0x20,  /* get character from keyboard, not echoed onto the terminal */
	TRAP_OUT   = 0x21,  /* output a character */
	TRAP_PUTS  = 0x22,  /* output a word string */
	TRAP_IN    = 0x23,  /* get character from keyboard, echoed onto the terminal */
	TRAP_PUTSP = 0x24,  /* output a byte string */
	TRAP_HALT  = 0x25   /* halt the program */
};


/*** Memory Storage ***/
/* 
 * 16-bits
 * 65536 memory locations
 * 128kb
 */
uint16_t memory[UINT16_MAX];


/*** Register Storage ***/
/* 
 * 10 registers
 * 8 general purpose registers (R0 - R7)
 * 1 program counter (PC)
 * 1 condition flags (COND)
 */
enum
{
	R_R0 = 0,
	R_R1,
	R_R2,
	R_R3,
	R_R4,
	R_R5,
	R_R6,
	R_R7,
	R_PC,	/* program counter */
	R_COND,
	R_COUNT
};

uint16_t reg[R_COUNT];

/*** Instruction Set ***/
/* 
 * 16-bits instruction
 * 16 opcodes
 * -------------------------
 * |  0000  | 000000000000 |
 * |------------------------
 * | opcode |  parameters  |
 * -------------------------
 */
enum
{
	OP_BR = 0, /* branch                 */
	OP_ADD,    /* add                    */
	OP_LD,     /* load                   */
	OP_ST,     /* store                  */
	OP_JSR,    /* jump register          */
	OP_AND,    /* bitwise and            */
	OP_LDR,    /* load register          */
	OP_STR,    /* store register         */
	OP_RTI,    /* unused                 */
	OP_NOT,    /* bitwise not            */
	OP_LDI,    /* load indirect          */
	OP_STI,    /* store indirect         */
	OP_JMP,    /* jump                   */
	OP_RES,    /* reserved (unused)      */
	OP_LEA,    /* load effective address */
	OP_TRAP    /* execute trap           */
};

/*** Condition Flags ***/
/*
 * P -- positive (greater than zero)
 * Z -- zero
 * N -- negative
 */
enum
{
	FL_POS = 1 << 0,  /* P */
	FL_ZRO = 1 << 1,  /* Z */
	FL_NEG = 1 << 2   /* N */
};


/*** Functions ***/
uint16_t sign_extend(uint16_t x, int bit_count)
{
	if ((x >> (bit_count - 1)) & 1) {
		x |= (0xFFFF << bit_count);
	}
	return x;
}

/* little endian to big endian */
uint16_t swap16(uint16_t x)
{
	return (x << 8) | (x >> 8);
}

void update_flags(uint16_t r)
{
	if (reg[r] == 0) {
		reg[R_COND] = FL_ZRO;
	} else if (reg[r] >> 15) {	/* a 1 int the left-most bit indicates negative */
		reg[R_COND] = FL_NEG;
	} else {
		reg[R_COND] = FL_POS;
	}
}

/* reading program into memory */
void read_image_file(FILE *file)
{
	/* the origin tells us where in memory to place the image */
	uint16_t origin;
	fread(&origin, sizeof(origin), 1, file);
	origin = swap16(origin);

	/* we know the maximum file size so we only need one fread */
	uint16_t max_read = UINT16_MAX - origin;
	uint16_t *p = memory + origin;
	size_t read = fread(p, sizeof(uint16_t), max_read, file);

	/* swap to little endian */
	while (read-- > 0) {
		*p = swap16(*p);
		p++;
	}
}

int read_image(const char *image_path)
{
	FILE *file = fopen(image_path, "rb");

	if (!file) {
		return 0;
	}

	read_image_file(file);
	fclose(file);
	return 1;
}

uint16_t check_key()
{
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(STDIN_FILENO, &readfds);

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

/*** Memory Access ***/
void mem_write(uint16_t address, uint16_t value)
{
	memory[address] = value;
}

uint16_t mem_read(uint16_t address)
{
	if (address == MR_KBSR) {
		if (check_key()) {
			memory[MR_KBSR] = (1 << 15);
			memory[MR_KBDR] = getchar();
		} else {
			memory[MR_KBSR] = 0;
		}
	}
	return memory[address];
}

/*** UNIX Setting Up Terminal Input Buffering ***/
struct termios original_tio;

void disable_input_buffering()
{
	tcgetattr(STDIN_FILENO, &original_tio);
	struct termios new_tio = original_tio;
	new_tio.c_lflag &= ~ICANON & ~ECHO;
	tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering()
{
	tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

void handle_interrupt(int signal)
{
	restore_input_buffering();
	printf("\n");
	exit(-2);
}

/*** ADD ***/
void op_ADD(uint16_t instr)
{
	/* destination register (DR) */
	uint16_t  DR = (instr >> 9) & 0x7;
	/* first operand (SR1) */
	uint16_t SR1 = (instr >> 6) & 0x7;
	/* whether we are in immediate mode */
	uint16_t imm_flag = (instr >> 5) & 0x1;

	if (imm_flag) {
		uint16_t imm5 = sign_extend(instr & 0x1F, 5);
		reg[DR] = reg[SR1] + imm5;
	} else {
		uint16_t SR2 = instr & 0x7;
		reg[DR] = reg[SR1] + reg[SR2];
	}

	update_flags(DR);
}

/*** AND ***/
void op_AND(uint16_t instr)
{
	/* destination register (DR) */
	uint16_t  DR = (instr >> 9) & 0x7;
	/* first operand (SR1) */
	uint16_t SR1 = (instr >> 6) & 0x7;
	/* whether we are in immediate mode */
	uint16_t imm_flag = (instr >> 5) & 0x1;

	if (imm_flag) {
		uint16_t imm5 = sign_extend(instr & 0x1F, 5);
		reg[DR] = reg[SR1] & imm5;
	} else {
		uint16_t SR2 = instr & 0x7;
		reg[DR] = reg[SR1] & reg[SR2];
	}

	update_flags(DR);
}

/*** NOT ***/
void op_NOT(uint16_t instr)
{
	/* destination register (DR) */
	uint16_t DR = (instr >> 9) & 0x7;
	/* source register (SR) */
	uint16_t SR = (instr >> 6) & 0x7;

	reg[DR] = ~reg[SR];

	update_flags(DR);
}

/*** BR ***/
void op_BR(uint16_t instr)
{
	/* PCoffset9 */
	uint16_t PC_offset = sign_extend(instr & 0x1ff, 9);
	/* conditional flags */
	uint16_t cond_flag = (instr >> 9) & 0x7;

	if (cond_flag & reg[R_COND]) {
		reg[R_PC] += PC_offset;
	}	
}

/*** JMP ***/
void op_JMP(uint16_t instr)
{
	/* base register */
	uint16_t BaseR = (instr >> 6) & 0x7;

	reg[R_PC] = reg[BaseR];
}

/*** JSR ***/
void op_JSR(uint16_t instr)
{
	reg[R_R7] = reg[R_PC];

/*JSR*/ if ((instr >> 11) & 1) {
		/* PCoffset11 */
		uint16_t PC_offset = sign_extend(instr & 0x7ff, 11);
		reg[R_PC] += PC_offset;
/*JSRR*/} else {
		/* base register */
		uint16_t BaseR = (instr >> 6) & 0x7;
		reg[R_PC] = reg[BaseR];
	}
}

/*** LD ***/
void op_LD(uint16_t instr)
{
	/* destination register (DR) */
	uint16_t DR = (instr >> 9) & 0x7;
	/* PCoffset9 */
	uint16_t PC_offset = sign_extend(instr & 0x1ff, 9);

	/* add pc_offset to the current PC, look at that memory location to get the final value */
	reg[DR] = mem_read(reg[R_PC] + PC_offset);

	update_flags(DR);
}

/*** LDI ***/
void op_LDI(uint16_t instr)
{
	/* destination register (DR) */
	uint16_t DR = (instr >> 9) & 0x7;
	/* PCoffset9 */
	uint16_t PC_offset = sign_extend(instr & 0x1ff, 9);

	/* add pc_offset to the current PC, look at that memory location to get the final address */
	reg[DR] = mem_read(mem_read(reg[R_PC] + PC_offset));

	update_flags(DR);
}

/*** LDR ***/
void op_LDR(uint16_t instr)
{
	/* destination register (DR) */
	uint16_t     DR = (instr >> 9) & 0x7;
	/* base register (BaseR) */
	uint16_t  BaseR = (instr >> 6) & 0x7;
	/* offset6 */
	uint16_t offset = sign_extend(instr & 0x3f, 6);

	/* add offset to the value of base register, look at that memory location to get the final value */
	reg[DR] = mem_read(reg[BaseR] + offset);

	update_flags(DR);
}

/*** LEA ***/
void op_LEA(uint16_t instr)
{
	/* destination register (DR) */
	uint16_t DR = (instr >> 9) & 0x7;
	/* PCoffset9 */
	uint16_t PC_offset = sign_extend(instr & 0x1ff, 9);

	/* add pc_offset to the current PC, and load this address into destination register */
	reg[DR] = reg[R_PC] + PC_offset;

	update_flags(DR);
}

/*** ST ***/
void op_ST(uint16_t instr)
{
	/* source register */
	uint16_t SR = (instr >> 9) & 0x7;
	/* PCoffset9 */
	uint16_t PC_offset = sign_extend(instr & 0x1ff, 9);

	mem_write(reg[R_PC] + PC_offset, reg[SR]);
}

/*** STI ***/
void op_STI(uint16_t instr)
{
	/* source register */
	uint16_t SR = (instr >> 9) & 0x7;
	/* PCoffset9 */
	uint16_t PC_offset = sign_extend(instr & 0x1ff, 9);

	mem_write(mem_read(reg[R_PC] + PC_offset), reg[SR]);
}

/*** STR ***/
void op_STR(uint16_t instr)
{
	/* source register */
	uint16_t    SR = (instr >> 9) & 0x7;
	/* base register */
	uint16_t BaseR = (instr >> 6) & 0x7;
	/* offset6 */
	uint16_t offset = sign_extend(instr & 0x3f, 6);

	mem_write(reg[BaseR] + offset, reg[SR]);
}

/*** TRAP ***/
void op_TRAP(uint16_t instr)
{
	/* trapvect8 */
	uint16_t trap_vect = sign_extend(instr & 0xff, 8);

	reg[R_R7] = reg[R_PC];
	reg[R_PC] = mem_read(trap_vect);
}


/*** TRAP_GETC ***/
void trap_GETC()
{
	reg[R_R0] = (uint16_t) getchar();
}

/*** TRAP_OUT ***/
void trap_OUT()
{
	putc((char) reg[R_R0], stdout);
	fflush(stdout);
}

/*** TRAP_PUTS ***/
void trap_PUTS()
{
	/* one char per word */
	uint16_t *c = memory + reg[R_R0];

	while (*c) {
		putc((char) *c, stdout);
		c++;
	}
	fflush(stdout);
}

/*** TRAP_IN ***/
void trap_IN()
{
	char c;

	printf("Enter a character: ");
	c = getchar();

	reg[R_R0] = (uint16_t) c;
	putc(c, stdout);
}

/*** TRAP_PUTSP ***/
void trap_PUTSP()
{
	/* two characters per word */
	uint16_t *c = memory + reg[R_R0];

	while (*c) {
		/* first character is bits[7:0] */
		putc((char) (*c & 0xff), stdout);

		if ((*c >> 8) == 0)
			break;

		/* second character is bits[15:8] */
		putc((char) ((*c >> 8 ) & 0xff), stdout);
		c++;
	}
	fflush(stdout);
}

/*** TRAP_HALT ***/
void trap_HALT(int *running)
{
	puts("HALT");
	fflush(stdout);
	*running = 0;
}

int main(int argc, char *argv[])
{
	/* show usage string */
	if (argc < 2) {
		printf("LC3 [image-file1] ...\n");
		exit(2);
	}

	for (int i = 1; i < argc; i++) {
		if (!read_image(argv[i])) {
			printf("Failed to load image: %s\n", argv[i]);
			exit(1);
		}
	}

	/* setup */
	signal(SIGINT, handle_interrupt);
	disable_input_buffering();

	/* set the PC to starting position */
	/* 0x3000 is the default           */

	enum { PC_START = 0x3000 };
	reg[R_PC] = PC_START;

	int running = 1;
	while (running) {
		/* FETCH */
		uint16_t instr = mem_read(reg[R_PC]++);
		uint16_t op = instr >> 12;

		switch (op) {
		case OP_ADD:
			op_ADD(instr);
			break;
		case OP_AND:
			op_AND(instr);
			break;
		case OP_NOT:
			op_NOT(instr);
			break;
		case OP_BR:
			op_BR(instr);
			break;
		case OP_JMP:
			op_JMP(instr);
			break;
		case OP_JSR:
			op_JSR(instr);
			break;
		case OP_LD:
			op_LD(instr);
			break;
		case OP_LDI:
			op_LDI(instr);
			break;
		case OP_LDR:
			op_LDR(instr);
			break;
		case OP_LEA:
			op_LEA(instr);
			break;
		case OP_ST:
			op_ST(instr);
			break;
		case OP_STI:
			op_STI(instr);
			break;
		case OP_STR:
			op_STR(instr);
			break;
		case OP_TRAP:
			switch (instr & 0xFF) {
			case TRAP_GETC:
				trap_GETC();
				break;
			case TRAP_OUT:
				trap_OUT();
				break;
			case TRAP_PUTS:
				trap_PUTS();
				break;
			case TRAP_IN:
				trap_IN();
				break;
			case TRAP_PUTSP:
				trap_PUTSP();
				break;
			case TRAP_HALT:
				trap_HALT(&running);
				break;
			}
			break;
		case OP_RES:
		case OP_RTI:
		default:
			abort();
			break;
		}
	}

	/* shutdown */
	restore_input_buffering();

	return 0;
}
