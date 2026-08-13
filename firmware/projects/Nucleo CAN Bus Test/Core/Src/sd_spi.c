/**
 * sd_spi.c - SD card over SPI1, direct register access.
 */

#include "sd_spi.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* SD command set                                                      */
/* ------------------------------------------------------------------ */

#define CMD0    0u    /* GO_IDLE_STATE            */
#define CMD8    8u    /* SEND_IF_COND             */
#define CMD9    9u    /* SEND_CSD                 */
#define CMD12  12u    /* STOP_TRANSMISSION        */
#define CMD16  16u    /* SET_BLOCKLEN             */
#define CMD17  17u    /* READ_SINGLE_BLOCK        */
#define CMD24  24u    /* WRITE_BLOCK              */
#define CMD55  55u    /* APP_CMD                  */
#define CMD58  58u    /* READ_OCR                 */
#define ACMD41 41u    /* SD_SEND_OP_COND          */

#define R1_IDLE          0x01u
#define R1_ILLEGAL_CMD   0x04u

#define TOKEN_START      0xFEu
#define DATA_ACCEPTED    0x05u

/* Timeouts in milliseconds. */
#define TMO_INIT     2000u
#define TMO_CMD       500u
#define TMO_READ      300u
#define TMO_WRITE     600u

static sd_type_t s_type;
static uint32_t  s_blocks;

/* ------------------------------------------------------------------ */
/* SPI primitives                                                      */
/* ------------------------------------------------------------------ */

/*
 * Baud rate is PCLK2 / 2^(BR+1), and PCLK2 is 80 MHz on this clock tree.
 *
 *   BR = 7 -> 312.5 kHz   card initialisation must be 100-400 kHz
 *   BR = 2 -> 10 MHz      run speed; conservative for jumper wiring
 *
 * 10 MHz rather than the 25 MHz the card would allow: SD-over-SPI on
 * breadboard jumpers is where signal integrity problems start showing up as
 * intermittent CRC errors, and the log only needs 104 bytes/second.
 */
#define SPI_BR_SLOW  7u
#define SPI_BR_FAST  2u

static void spi_set_br(uint32_t br)
{
    uint32_t cr1 = SPI1->CR1;
    cr1 &= ~SPI_CR1_SPE;
    SPI1->CR1 = cr1;

    cr1 &= ~SPI_CR1_BR_Msk;
    cr1 |= (br << SPI_CR1_BR_Pos);

    SPI1->CR1 = cr1 | SPI_CR1_SPE;
}

static uint8_t spi_xfer(uint8_t out)
{
    /*
     * The 8-bit access to DR is essential. A 32-bit write would push two bytes
     * into the FIFO on L4 parts, because the data register packs frames
     * smaller than 16 bits. This is the classic L4 SPI bug and it presents as
     * a card that never leaves idle state.
     */
    while ((SPI1->SR & SPI_SR_TXE) == 0u) { }

    *(volatile uint8_t *)&SPI1->DR = out;

    while ((SPI1->SR & SPI_SR_RXNE) == 0u) { }

    return *(volatile uint8_t *)&SPI1->DR;
}

static void cs_low(void)
{
    HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_RESET);
}

static void cs_high(void)
{
    HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_SET);
    /* The card needs eight clocks after CS rises to finish its business. */
    (void)spi_xfer(0xFFu);
}

/* ------------------------------------------------------------------ */
/* Card protocol                                                       */
/* ------------------------------------------------------------------ */

/** Clock out 0xFF until the card stops holding the line low. */
static bool wait_ready(uint32_t timeout_ms)
{
    uint32_t deadline = HAL_GetTick() + timeout_ms;

    do {
        if (spi_xfer(0xFFu) == 0xFFu)
        {
            return true;
        }
    } while ((int32_t)(HAL_GetTick() - deadline) < 0);

    return false;
}

/**
 * Send a command and return the R1 response.
 *
 * CRC is only checked by the card for CMD0 and CMD8 (before CRC is turned off),
 * so those two carry hardcoded correct values and everything else sends a
 * dummy. This is standard practice and is why the constants look magic.
 */
static uint8_t send_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t crc = 0x01u;   /* stop bit; valid CRC not required */

    if (cmd == CMD0) crc = 0x95u;
    if (cmd == CMD8) crc = 0x87u;

    (void)spi_xfer(0xFFu);

    spi_xfer((uint8_t)(0x40u | cmd));
    spi_xfer((uint8_t)(arg >> 24));
    spi_xfer((uint8_t)(arg >> 16));
    spi_xfer((uint8_t)(arg >> 8));
    spi_xfer((uint8_t)arg);
    spi_xfer(crc);

    /* R1 arrives within 8 bytes; the top bit is clear on a valid response. */
    uint8_t r1 = 0xFFu;

    for (int i = 0; i < 10; i++)
    {
        r1 = spi_xfer(0xFFu);
        if ((r1 & 0x80u) == 0u)
        {
            break;
        }
    }

    return r1;
}

static uint8_t send_acmd(uint8_t cmd, uint32_t arg)
{
    (void)send_cmd(CMD55, 0);
    return send_cmd(cmd, arg);
}

/** Read a data block of @p len bytes preceded by the 0xFE start token. */
static bool read_data(uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    uint32_t deadline = HAL_GetTick() + timeout_ms;
    uint8_t  token;

    do {
        token = spi_xfer(0xFFu);
        if (token != 0xFFu)
        {
            break;
        }
    } while ((int32_t)(HAL_GetTick() - deadline) < 0);

    if (token != TOKEN_START)
    {
        return false;
    }

    for (uint32_t i = 0; i < len; i++)
    {
        buf[i] = spi_xfer(0xFFu);
    }

    /* Discard the 16-bit CRC. SPI mode has CRC checking off by default. */
    (void)spi_xfer(0xFFu);
    (void)spi_xfer(0xFFu);

    return true;
}

/* ------------------------------------------------------------------ */
/* Hardware bring-up                                                   */
/* ------------------------------------------------------------------ */

static void spi_hw_init(void)
{
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};

    /* PA5 SCK, PA7 MOSI - alternate function 5 */
    g.Pin       = GPIO_PIN_5 | GPIO_PIN_7;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &g);

    /* PA6 MISO - pull-up, see the note in sd_spi.h */
    g.Pin  = GPIO_PIN_6;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &g);

    /* PB6 CS - plain output, idle high */
    g.Pin       = SD_CS_PIN;
    g.Mode      = GPIO_MODE_OUTPUT_PP;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = 0;
    HAL_GPIO_Init(SD_CS_PORT, &g);

    HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_SET);

    SPI1->CR1 = 0u;

    /* 8-bit frames, RXNE asserted on 8 bits rather than 16. Getting FRXTH
       wrong makes every read block forever waiting for a second byte. */
    SPI1->CR2 = SPI_CR2_FRXTH
              | SPI_CR2_DS_0 | SPI_CR2_DS_1 | SPI_CR2_DS_2;   /* DS = 0b0111 */

    SPI1->CR1 = (SPI_BR_SLOW << SPI_CR1_BR_Pos)
              | SPI_CR1_MSTR
              | SPI_CR1_SSI
              | SPI_CR1_SSM;      /* CPOL = 0, CPHA = 0 */

    SPI1->CR1 |= SPI_CR1_SPE;
}

bool sd_spi_init(void)
{
    s_type   = SD_TYPE_NONE;
    s_blocks = 0u;

    spi_hw_init();
    spi_set_br(SPI_BR_SLOW);

    /*
     * At least 74 clocks with CS and MOSI held high put the card into a known
     * state. Ten bytes gives 80. Skipping this works often enough to be
     * misleading and fails on exactly the card you care about.
     */
    HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_SET);
    HAL_Delay(2);

    for (int i = 0; i < 10; i++)
    {
        (void)spi_xfer(0xFFu);
    }

    cs_low();

    /* --- CMD0: enter idle state --- */
    bool idle = false;
    uint32_t deadline = HAL_GetTick() + TMO_INIT;

    do {
        if (send_cmd(CMD0, 0) == R1_IDLE)
        {
            idle = true;
            break;
        }
    } while ((int32_t)(HAL_GetTick() - deadline) < 0);

    if (!idle)
    {
        cs_high();
        return false;
    }

    /* --- CMD8: distinguish v2 from v1 --- */
    bool v2 = false;
    uint8_t r1 = send_cmd(CMD8, 0x000001AAu);

    if (r1 == R1_IDLE)
    {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++)
        {
            r7[i] = spi_xfer(0xFFu);
        }

        /* The card echoes the voltage class and check pattern back. */
        if (r7[2] == 0x01u && r7[3] == 0xAAu)
        {
            v2 = true;
        }
        else
        {
            cs_high();
            return false;   /* card rejects 2.7-3.6V operation */
        }
    }
    else if ((r1 & R1_ILLEGAL_CMD) == 0u)
    {
        cs_high();
        return false;       /* unexpected response, not a v1 card either */
    }

    /* --- ACMD41: leave idle. HCS bit only meaningful for v2. --- */
    deadline = HAL_GetTick() + TMO_INIT;
    bool ready = false;

    do {
        if (send_acmd(ACMD41, v2 ? 0x40000000u : 0u) == 0u)
        {
            ready = true;
            break;
        }
    } while ((int32_t)(HAL_GetTick() - deadline) < 0);

    if (!ready)
    {
        cs_high();
        return false;
    }

    s_type = SD_TYPE_SDSC;

    /* --- CMD58: block or byte addressing? --- */
    if (v2)
    {
        if (send_cmd(CMD58, 0) != 0u)
        {
            cs_high();
            return false;
        }

        uint8_t ocr[4];
        for (int i = 0; i < 4; i++)
        {
            ocr[i] = spi_xfer(0xFFu);
        }

        /* CCS, bit 30 of the OCR: set means addresses are block numbers. */
        if (ocr[0] & 0x40u)
        {
            s_type = SD_TYPE_SDHC;
        }
    }

    /*
     * Byte-addressed cards need an explicit 512-byte block length. Block
     * addressed cards are fixed at 512 and reject CMD16, so only send it when
     * it applies.
     */
    if (s_type == SD_TYPE_SDSC)
    {
        if (send_cmd(CMD16, FAT32_SECTOR_SIZE) != 0u)
        {
            cs_high();
            return false;
        }
    }

    /* --- CMD9: capacity, for reporting only --- */
    if (send_cmd(CMD9, 0) == 0u)
    {
        uint8_t csd[16];

        if (read_data(csd, sizeof(csd), TMO_READ))
        {
            if ((csd[0] >> 6) == 1u)
            {
                /* CSD v2: capacity = (C_SIZE + 1) * 512 KB */
                uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16)
                                | ((uint32_t)csd[8] << 8)
                                | csd[9];
                s_blocks = (c_size + 1u) * 1024u;
            }
            else
            {
                /* CSD v1 */
                uint32_t c_size = (((uint32_t)(csd[6] & 0x03u) << 10)
                                | ((uint32_t)csd[7] << 2)
                                | ((uint32_t)csd[8] >> 6));
                uint32_t mult   = (uint32_t)(((csd[9] & 0x03u) << 1)
                                | ((csd[10] & 0x80u) >> 7));
                uint32_t rdblen = csd[5] & 0x0Fu;

                s_blocks = (c_size + 1u) * (1u << (mult + 2u))
                         * ((1u << rdblen) / FAT32_SECTOR_SIZE);
            }
        }
    }

    cs_high();
    spi_set_br(SPI_BR_FAST);

    return true;
}

sd_type_t sd_spi_type(void)      { return s_type; }
uint32_t  sd_spi_block_count(void) { return s_blocks; }

/* ------------------------------------------------------------------ */
/* Block IO                                                            */
/* ------------------------------------------------------------------ */

/** SDHC addresses in blocks, SDSC in bytes. */
static uint32_t to_addr(uint32_t lba)
{
    return (s_type == SD_TYPE_SDHC) ? lba : lba * FAT32_SECTOR_SIZE;
}

int sd_spi_read_block(uint32_t lba, uint8_t *buf)
{
    if (s_type == SD_TYPE_NONE)
    {
        return 1;
    }

    cs_low();

    if (!wait_ready(TMO_CMD))
    {
        cs_high();
        return 1;
    }

    if (send_cmd(CMD17, to_addr(lba)) != 0u)
    {
        cs_high();
        return 1;
    }

    bool ok = read_data(buf, FAT32_SECTOR_SIZE, TMO_READ);

    cs_high();
    return ok ? 0 : 1;
}

int sd_spi_write_block(uint32_t lba, const uint8_t *buf)
{
    if (s_type == SD_TYPE_NONE)
    {
        return 1;
    }

    cs_low();

    if (!wait_ready(TMO_CMD))
    {
        cs_high();
        return 1;
    }

    if (send_cmd(CMD24, to_addr(lba)) != 0u)
    {
        cs_high();
        return 1;
    }

    (void)spi_xfer(0xFFu);          /* one byte gap before the token */
    spi_xfer(TOKEN_START);

    for (uint32_t i = 0; i < FAT32_SECTOR_SIZE; i++)
    {
        spi_xfer(buf[i]);
    }

    (void)spi_xfer(0xFFu);          /* dummy CRC */
    (void)spi_xfer(0xFFu);

    uint8_t resp = spi_xfer(0xFFu);

    if ((resp & 0x1Fu) != DATA_ACCEPTED)
    {
        cs_high();
        return 1;
    }

    /*
     * The card pulls MISO low while it programs the block. This can take
     * hundreds of milliseconds on a cheap card doing wear levelling, which is
     * exactly why the CAN receive path is interrupt-driven and buffered: the
     * main loop is allowed to stall here without losing bus traffic.
     */
    if (!wait_ready(TMO_WRITE))
    {
        cs_high();
        return 1;
    }

    cs_high();
    return 0;
}

const fat32_bdev_t sd_spi_bdev = {
    sd_spi_read_block,
    sd_spi_write_block,
};
