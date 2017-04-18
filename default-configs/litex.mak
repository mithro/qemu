# LiteX config
CONFIG_LITEX=y

# LiteX UART enabled via CONFIG_LITEX=y
# LiteX Timer enabled via CONFIG_LITEX=y
CONFIG_PTIMER=y

# LiteX I2C support
CONFIG_BITBANG_I2C=y
CONFIG_LITEX_I2C=y
# smbus_eeprom
# CONFIG_DCC=y

# LiteX SPI support
CONFIG_SSI=y
CONFIG_BITBANG_SPI=y
CONFIG_LITEX_SPI=y
CONFIG_SSI_M25P80=y

# LiteX SDCard via SPI support
CONFIG_SSI_SD=y
CONFIG_SD=y
