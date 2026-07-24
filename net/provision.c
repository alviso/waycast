#include "provision.h"

static const vmesh_wifi_ops_t *s_wifi;
static const vmesh_tiledl_ops_t *s_tiledl;
static const vmesh_fw_ops_t *s_fw;

void vmesh_wifi_set_ops(const vmesh_wifi_ops_t *ops) { s_wifi = ops; }
const vmesh_wifi_ops_t *vmesh_wifi_ops(void) { return s_wifi; }
void vmesh_tiledl_set_ops(const vmesh_tiledl_ops_t *ops) { s_tiledl = ops; }
const vmesh_tiledl_ops_t *vmesh_tiledl_ops(void) { return s_tiledl; }
void vmesh_fw_set_ops(const vmesh_fw_ops_t *ops) { s_fw = ops; }
const vmesh_fw_ops_t *vmesh_fw_ops(void) { return s_fw; }
