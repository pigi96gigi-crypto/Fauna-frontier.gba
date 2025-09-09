
export DEVKITPRO ?= /opt/devkitpro
export DEVKITARM ?= $(DEVKITPRO)/devkitARM
include $(DEVKITPRO)/libgba/gba_rules

TARGET := faunafrontier
BUILD  := build
SOURCES := source
INCLUDES := include

.PHONY: all
all: $(TARGET).gba
