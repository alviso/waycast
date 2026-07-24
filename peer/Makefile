# vmesh_peer — build ON the Raspberry Pi, against Semtech's sx1302_hal.
#
#   git clone https://github.com/Lora-net/sx1302_hal.git
#   (cd sx1302_hal && make libloragw)
#   make SX1302_HAL=../sx1302_hal
#
# Reset the RAK2287 before first run:
#   sx1302_hal/tools/reset_lgw.sh start   (RAK Pi HAT: reset GPIO17)

SX1302_HAL ?= $(HOME)/sx1302_hal

CC := gcc
CFLAGS := -std=c11 -O2 -Wall -D_DEFAULT_SOURCE \
	-I../msg -I../mesh -I../net \
	-I$(SX1302_HAL)/libloragw/inc
LDFLAGS := -L$(SX1302_HAL)/libloragw -lloragw \
	-L$(SX1302_HAL)/libtools -ltinymt32 \
	-lm -lpthread -lrt

vmesh_peer: vmesh_peer.c ../mesh/vmesh_mesh.c ../msg/vmesh_wire.c ../net/lora_dtu.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

clean:
	rm -f vmesh_peer

# --- SX1301 variant (RAK2245-class, legacy lora_gateway HAL) ---
LORA_GATEWAY ?= $(HOME)/lora_gateway
vmesh_peer_sx1301: vmesh_peer_sx1301.c ../mesh/vmesh_mesh.c ../msg/vmesh_wire.c ../net/lora_dtu.c
	$(CC) -std=c11 -O2 -Wall -D_DEFAULT_SOURCE -I../msg -I../mesh -I../net \
		-I$(LORA_GATEWAY)/libloragw/inc \
		$^ -L$(LORA_GATEWAY)/libloragw -lloragw -lrt -lm -o $@
