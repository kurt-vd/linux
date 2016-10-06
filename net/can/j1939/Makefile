
obj-$(CONFIG_CAN_J1939)	+= can-j1939.o

can-j1939-objs := main.o \
	bus.o \
	address-claim.o transport.o \
	socket.o

ifeq ($(CONFIG_CAN_J1939_DEBUG),y)
	EXTRA_CFLAGS += -DDEBUG
endif

