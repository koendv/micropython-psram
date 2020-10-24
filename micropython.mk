###############################################################################
# psram

ifeq ($(MODULE_PSRAM_ENABLED),1)
PSRAM_DIR := $(USERMOD_DIR)
INC += \
  -I$(PSRAM_DIR)/include

SRC_MOD += $(addprefix $(PSRAM_DIR)/,\
  mod_psram.c \
  psram.c \
	)

endif
