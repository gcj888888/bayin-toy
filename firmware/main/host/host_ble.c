#include "host_ble.h"

#include "host_link.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char *TAG = "ble";

/* 电脑端 Web Bluetooth 就是按这个名字前缀（EasyInput）筛设备的。 */
#define BLE_DEV_NAME "EasyInput-Piano"

/* Nordic UART Service（NUS）UUID：
 *   服务    6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX 写   6E400002-...（电脑 → 板子）
 *   TX 通知 6E400003-...（板子 → 电脑）
 * BLE_UUID128_INIT 的字节序是小端（低字节在前），所以下面按反序填。 */
static const ble_uuid128_t kSvcUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t kRxUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t kTxUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle;
static bool s_subscribed;

/* 单生产者（NimBLE 主机任务）／单消费者（主循环）环形缓冲：
 * BLE 回调只负责"入环"，命令统一由主循环 host_ble_poll() 取走再解析。 */
#define BT_RX_RING 2048
static volatile uint8_t s_ring[BT_RX_RING];
static volatile uint32_t s_ring_w, s_ring_r;

static void ring_put(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const uint32_t nw = (s_ring_w + 1u) % BT_RX_RING;
        if (nw == s_ring_r) {
            return; /* 满：直接丢弃（绝不阻塞 BLE 回调） */
        }
        s_ring[s_ring_w] = d[i];
        s_ring_w = nw;
    }
}

static int ring_get(uint8_t *out, size_t max)
{
    size_t i = 0;
    while (s_ring_r != s_ring_w && i < max) {
        out[i++] = s_ring[s_ring_r];
        s_ring_r = (s_ring_r + 1u) % BT_RX_RING;
    }
    return (int)i;
}

static int gatt_chr_access_rx(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t buf[160];
    uint16_t out_len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len) == 0 && out_len > 0) {
        ring_put(buf, out_len); /* 只入环，主循环再解析（与 USB 同一上下文） */
    }
    return 0;
}

static int gatt_chr_access_tx(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_UNLIKELY; /* TX 特征只做 notify，不可读 */
}

static const struct ble_gatt_svc_def kGattSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = &kRxUuid.u,
                 .access_cb = gatt_chr_access_rx,
                 .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
                {.uuid = &kTxUuid.u,
                 .access_cb = gatt_chr_access_tx,
                 .flags = BLE_GATT_CHR_F_NOTIFY},
                {0}},
    },
    {0}};

static void ble_advertise(void);

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "central connected (conn=%u)", (unsigned)s_conn_handle);
        } else {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason=%d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        ble_advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        s_subscribed = (event->subscribe.cur_notify != 0);
        ESP_LOGI(TAG, "subscribe notify=%d", (int)s_subscribed);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertise();
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update = %u", (unsigned)event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)BLE_DEV_NAME;
    fields.name_len = (uint8_t)strlen(BLE_DEV_NAME);
    fields.name_is_complete = 1;
    (void)ble_gap_adv_set_fields(&fields);

    /* 128 位服务 UUID 放扫描响应里（广播包只放名字，31 字节装不下两者） */
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = (ble_uuid128_t *)&kSvcUuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    (void)ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    (void)ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_cb,
                            NULL);
}

static void ble_on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "ensure_addr failed");
        return;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "infer_auto failed");
        return;
    }
    (void)ble_gatts_find_chr(&kSvcUuid.u, &kTxUuid.u, NULL, &s_tx_val_handle);
    ble_advertise();
    ESP_LOGI(TAG, "advertising as '%s'", BLE_DEV_NAME);
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "stack reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t host_ble_init(void)
{
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(kGattSvcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(kGattSvcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        return ESP_FAIL;
    }

    (void)ble_svc_gap_device_name_set(BLE_DEV_NAME);

    /* 板子往外发的每一行 JSON 同时走 BLE（USB 侧不受影响） */
    host_link_set_sink(host_ble_notify);
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE(NUS) ready");
    return ESP_OK;
}

void host_ble_poll(void)
{
    uint8_t buf[96];
    int n;
    while ((n = ring_get(buf, sizeof(buf))) > 0) {
        host_link_feed_rx((const char *)buf, (size_t)n);
    }
}

void host_ble_notify(const char *line)
{
    if (line == NULL || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_subscribed ||
        s_tx_val_handle == 0) {
        return;
    }

    const size_t len = strlen(line);
    uint16_t mtu = ble_att_mtu(s_conn_handle);
    size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;
    if (chunk > 180) {
        chunk = 180;
    }

    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > chunk) {
            n = chunk;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(line + off, (uint16_t)n);
        if (om == NULL) {
            break;
        }
        /* 注意：ble_gatts_notify_custom 会接管 om 的所有权，失败也不要手动 free */
        if (ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om) != 0) {
            break;
        }
        off += n;
    }

    /* 末尾补一个换行：浏览器侧按 '\n' 重组，与 USB 侧的换行 JSON 完全一致 */
    struct os_mbuf *nl = ble_hs_mbuf_from_flat("\n", 1);
    if (nl != NULL) {
        (void)ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, nl);
    }
}
