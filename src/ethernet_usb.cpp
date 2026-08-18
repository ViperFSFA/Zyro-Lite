// ETHERNET Has not been tested, verified, or used. maybe use this in future versions of the firmware
// Consider keeping a dev note

#include "ethernet_usb.h"
#include <Arduino.h>
#include "usb/usb_host.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_event.h"
#include "lwip/dhcp.h"

// tunables
#define ETH_USB_MAX_PACKET_SIZE   1518
#define ETH_USB_RX_BUFS           4
#define ETH_USB_TASK_STACK        4096
#define ETH_USB_TASK_PRIO         5

// CDC class-specific bits
#define CDC_SUBCLASS_ECM              0x06
#define CDC_FUNC_DESC_UNION            0x06
#define CDC_FUNC_DESC_ETHERNET          0x0F
#define CDC_REQ_SET_ETHERNET_PACKET_FILTER 0x43
#define ECM_FILTER_DIRECTED   0x01
#define ECM_FILTER_BROADCAST  0x02

static usb_host_client_handle_t sClientHdl = nullptr;
static usb_device_handle_t sDevHdl = nullptr;
static TaskHandle_t sHostTask = nullptr;
static TaskHandle_t sClientTask = nullptr;

static uint8_t sCommIf = 0xFF, sDataIf = 0xFF, sDataAltUp = 0xFF;
static uint8_t sEpBulkIn = 0, sEpBulkOut = 0;
static uint16_t sBulkInMps = 64, sBulkOutMps = 64;
static uint8_t sMac[6] = {0};

static usb_transfer_t *sRxXfer[ETH_USB_RX_BUFS] = {nullptr};

static esp_netif_t *sNetif = nullptr;
static volatile bool sAdapterPresent = false;
static volatile bool sLinkUp = false;
static volatile bool sHasIp = false;
static volatile bool sDirty = true;
static String sAdapterName = "-";
static char sIpStr[16] = "0.0.0.0";

static void debugPrint(const char *msg) {
    // Deliberately not Serial
    (void)msg;
}

// esp_netif <-> our driver glue (mirrors esp_eth's io glue pattern)
static esp_err_t netifTransmit(void *h, void *buffer, size_t len) {
    if (!sDevHdl || sEpBulkOut == 0 || len > ETH_USB_MAX_PACKET_SIZE) return ESP_FAIL;

    usb_transfer_t *xfer = nullptr;
    if (usb_host_transfer_alloc(len, 0, &xfer) != ESP_OK) return ESP_ERR_NO_MEM;

    memcpy(xfer->data_buffer, buffer, len);
    xfer->num_bytes = len;
    xfer->device_handle = sDevHdl;
    xfer->bEndpointAddress = sEpBulkOut;
    xfer->callback = [](usb_transfer_t *t) { usb_host_transfer_free(t); };
    xfer->context = nullptr;

    esp_err_t err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK) usb_host_transfer_free(xfer);
    return err;
}

static void netifFreeRxBuffer(void *h, void *buffer) {
    // We hand esp_netif a copy, so nothing to free here beyond the copy itself.
    free(buffer);
}

static void onRxTransfer(usb_transfer_t *xfer) {
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0 && sNetif) {
        void *copy = malloc(xfer->actual_num_bytes);
        if (copy) {
            memcpy(copy, xfer->data_buffer, xfer->actual_num_bytes);
            esp_netif_receive(sNetif, copy, xfer->actual_num_bytes, nullptr);
        }
    }
    // Re-arm for the next frame.
    if (sAdapterPresent) usb_host_transfer_submit(xfer);
}

static void startRxPipes() {
    for (int i = 0; i < ETH_USB_RX_BUFS; i++) {
        if (usb_host_transfer_alloc(ETH_USB_MAX_PACKET_SIZE, 0, &sRxXfer[i]) != ESP_OK) continue;
        sRxXfer[i]->device_handle = sDevHdl;
        sRxXfer[i]->bEndpointAddress = sEpBulkIn;
        sRxXfer[i]->num_bytes = ETH_USB_MAX_PACKET_SIZE;
        sRxXfer[i]->callback = onRxTransfer;
        usb_host_transfer_submit(sRxXfer[i]);
    }
}

// CDC-ECM descriptor parsing
static bool parseHexMac(const char *hex12, uint8_t out[6]) {
    if (strlen(hex12) < 12) return false;
    for (int i = 0; i < 6; i++) {
        char byteStr[3] = { hex12[i * 2], hex12[i * 2 + 1], 0 };
        out[i] = (uint8_t)strtoul(byteStr, nullptr, 16);
    }
    return true;
}

static bool claimCdcEcm(usb_device_handle_t dev) {
    const usb_config_desc_t *cfg = nullptr;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return false;

    int offset = 0;
    const usb_intf_desc_t *commIf = nullptr;
    uint8_t ethFuncStringIdx = 0xFF;

    for (int i = 0; i < cfg->bNumInterfaces * 2 && offset < cfg->wTotalLength; i++) {
        const usb_standard_desc_t *d = usb_parse_next_descriptor_of_type(
            (const usb_standard_desc_t *)cfg, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset);
        if (!d) break;
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;

        if (intf->bInterfaceClass == USB_CLASS_COMM && intf->bInterfaceSubClass == CDC_SUBCLASS_ECM) {
            commIf = intf;
            sCommIf = intf->bInterfaceNumber;

            int fdOffset = offset;
            while (fdOffset < cfg->wTotalLength) {
                const uint8_t *raw = ((const uint8_t *)cfg) + fdOffset;
                if (raw[1] != 0x24 /* CS_INTERFACE */) break; // ran past the functional descriptors
                uint8_t subtype = raw[2];
                if (subtype == CDC_FUNC_DESC_ETHERNET) {
                    ethFuncStringIdx = raw[3]; // iMACAddress
                } else if (subtype == CDC_FUNC_DESC_UNION) {
                    sDataIf = raw[4]; // bSubordinateInterface0
                }
                fdOffset += raw[0];
            }
        } else if (intf->bInterfaceClass == USB_CLASS_CDC_DATA) {
            // Remember an alt-setting >0 data interface with two bulk endpoints.
            // ECM data interfaces expose 0 endpoints at alt 0 (idle) and the
            // bulk pair at alt 1.
            if (intf->bNumEndpoints >= 2 && intf->bInterfaceNumber == sDataIf) {
                sDataAltUp = intf->bAlternateSetting;
                const usb_ep_desc_t *ep;
                int epOff = offset;
                for (int e = 0; e < intf->bNumEndpoints; e++) {
                    ep = (const usb_ep_desc_t *)usb_parse_next_descriptor_of_type(
                        (const usb_standard_desc_t *)cfg, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_ENDPOINT, &epOff);
                    if (!ep) break;
                    bool isIn = (ep->bEndpointAddress & 0x80) != 0;
                    if ((ep->bmAttributes & 0x03) == USB_BM_ATTRIBUTES_XFER_BULK) {
                        if (isIn) { sEpBulkIn = ep->bEndpointAddress; sBulkInMps = ep->wMaxPacketSize; }
                        else      { sEpBulkOut = ep->bEndpointAddress; sBulkOutMps = ep->wMaxPacketSize; }
                    }
                }
            }
        }
    }

    if (!commIf || sDataIf == 0xFF || sEpBulkIn == 0 || sEpBulkOut == 0) {
        debugPrint("CDC-ECM: no matching comm+data interface pair found (not a CDC-ECM adapter?)");
        return false;
    }

    if (usb_host_interface_claim(sClientHdl, dev, sCommIf, 0) != ESP_OK) return false;
    if (usb_host_interface_claim(sClientHdl, dev, sDataIf, sDataAltUp) != ESP_OK) return false;

    // Pull the MAC address string descriptor (ASCII hex, no separators) if we
    // found one; otherwise fall back to a locally-administered random MAC so
    // the interface still comes up (not ideal, but keeps things functional).
    bool gotMac = false;
    if (ethFuncStringIdx != 0xFF) {
        usb_transfer_t *xfer = nullptr;
        if (usb_host_transfer_alloc(256, 0, &xfer) == ESP_OK) {
            xfer->device_handle = dev;
            xfer->bEndpointAddress = 0; // control
            usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
            USB_SETUP_PACKET_INIT_GET_STR_DESC(setup, ethFuncStringIdx, 0x0409, 255);
            xfer->num_bytes = sizeof(usb_setup_packet_t) + 255;
            if (usb_host_transfer_submit_control(sClientHdl, xfer) == ESP_OK) {
                // NOTE: this is written as a synchronous-looking call for
                // clarity; in the real driver task this runs inside the
                // client event loop, so treat this helper as illustrative.
                // convert to the async callback pattern used for RX above
                // if hit timing issues during bring-up.
                char asciiMac[13] = {0};
                const uint8_t *strDesc = xfer->data_buffer + sizeof(usb_setup_packet_t);
                for (int i = 0; i < 12 && i < (strDesc[0] - 2) / 2; i++) {
                    asciiMac[i] = (char)strDesc[2 + i * 2];
                }
                gotMac = parseHexMac(asciiMac, sMac);
            }
            usb_host_transfer_free(xfer);
        }
    }
    if (!gotMac) {
        sMac[0] = 0x02; // locally
        for (int i = 1; i < 6; i++) sMac[i] = (uint8_t)esp_random();
        debugPrint("CDC-ECM: adapter gave no usable MAC string, using a random locally-administered MAC");
    }

    // Enable directed + broadcast frames (class-specific SET_ETHERNET_PACKET_FILTER).
    usb_transfer_t *filt = nullptr;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &filt) == ESP_OK) {
        filt->device_handle = dev;
        filt->bEndpointAddress = 0;
        usb_setup_packet_t *setup = (usb_setup_packet_t *)filt->data_buffer;
        setup->bmRequestType = 0x21; // host->device, class, interface
        setup->bRequest = CDC_REQ_SET_ETHERNET_PACKET_FILTER;
        setup->wValue = ECM_FILTER_DIRECTED | ECM_FILTER_BROADCAST;
        setup->wIndex = sCommIf;
        setup->wLength = 0;
        filt->num_bytes = sizeof(usb_setup_packet_t);
        usb_host_transfer_submit_control(sClientHdl, filt);
        usb_host_transfer_free(filt);
    }

    return true;
}

// USB host client event handling (new device / disconnect)
static void clientEventCb(const usb_host_client_event_msg_t *event, void *arg) {
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (usb_host_device_open(sClientHdl, event->new_dev.address, &sDevHdl) != ESP_OK) return;

        const usb_device_desc_t *devDesc = nullptr;
        usb_host_get_device_descriptor(sDevHdl, &devDesc);

        if (claimCdcEcm(sDevHdl)) {
            sAdapterPresent = true;
            sAdapterName = "USB Ethernet (CDC-ECM)";
            startRxPipes();

            // Bring the netif up now that we have a MAC + working pipes.
            esp_netif_set_mac(sNetif, sMac);
            esp_netif_action_start(sNetif, nullptr, 0, nullptr);
            esp_netif_action_connected(sNetif, nullptr, 0, nullptr);
            sLinkUp = true;
        } else {
            usb_host_device_close(sClientHdl, sDevHdl);
            sDevHdl = nullptr;
        }
        sDirty = true;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (sAdapterPresent) {
            esp_netif_action_stop(sNetif, nullptr, 0, nullptr);
            for (auto &x : sRxXfer) { if (x) { usb_host_transfer_free(x); x = nullptr; } }
            if (sDevHdl) usb_host_device_close(sClientHdl, sDevHdl);
            sDevHdl = nullptr;
            sAdapterPresent = false;
            sLinkUp = false;
            sHasIp = false;
            sAdapterName = "-";
            strcpy(sIpStr, "0.0.0.0");
        }
        sDirty = true;
    }
}

static void ipEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        auto *evt = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&evt->ip_info.ip, sIpStr, sizeof(sIpStr));
        sHasIp = true;
        sDirty = true;
    }
}

// Host + client library task loops
static void hostLibTask(void *arg) {
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

static void clientTask(void *arg) {
    while (true) {
        usb_host_client_handle_events(sClientHdl, portMAX_DELAY);
    }
}

// Public API
void ethernetInit() {
    if (sNetif) return; // already initialized

    // Give up the native-USB CDC console / device stack before switching
    // the shared PHY into host mode
    Serial.end();
    Serial.flush();

    esp_netif_config_t netifCfg = ESP_NETIF_DEFAULT_ETH();
    sNetif = esp_netif_new(&netifCfg);

    esp_netif_driver_ifconfig_t driverCfg = {
        .handle = nullptr,
        .transmit = netifTransmit,
        .driver_free_rx_buffer = netifFreeRxBuffer,
    };
    esp_netif_set_driver_config(sNetif, &driverCfg);

    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ipEventHandler, nullptr);

    usb_host_config_t hostCfg = {};
    hostCfg.skip_phy_setup = false;
    hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    usb_host_install(&hostCfg);
    xTaskCreate(hostLibTask, "usbh_lib", ETH_USB_TASK_STACK, nullptr, ETH_USB_TASK_PRIO, &sHostTask);

    usb_host_client_config_t clientCfg = {};
    clientCfg.is_synchronous = false;
    clientCfg.max_num_event_msg = 8;
    clientCfg.async.client_event_callback = clientEventCb;
    clientCfg.async.callback_arg = nullptr;
    usb_host_client_register(&clientCfg, &sClientHdl);
    xTaskCreate(clientTask, "usbh_client", ETH_USB_TASK_STACK, nullptr, ETH_USB_TASK_PRIO, &sClientTask);

    sDirty = true;
}

void ethernetTick() {
    // Enumeration/RX/TX all run on their own tasks above.
}

void ethernetShutdown() {
    if (!sNetif) return;

    if (sAdapterPresent) {
        esp_netif_action_stop(sNetif, nullptr, 0, nullptr);
        for (auto &x : sRxXfer) { if (x) { usb_host_transfer_free(x); x = nullptr; } }
        if (sDevHdl) usb_host_device_close(sClientHdl, sDevHdl);
        sDevHdl = nullptr;
    }

    if (sClientTask) { vTaskDelete(sClientTask); sClientTask = nullptr; }
    if (sClientHdl) { usb_host_client_deregister(sClientHdl); sClientHdl = nullptr; }
    if (sHostTask) { vTaskDelete(sHostTask); sHostTask = nullptr; }
    usb_host_uninstall();

    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ipEventHandler);
    esp_netif_destroy(sNetif);
    sNetif = nullptr;

    sAdapterPresent = false;
    sLinkUp = false;
    sHasIp = false;
    sAdapterName = "-";
    strcpy(sIpStr, "0.0.0.0");
    sDirty = true;

    // Hand the shared PHY back to device mode so Serial-over-USB returns.
    Serial.begin(115200);
}

bool ethernetAdapterPresent() { return sAdapterPresent; }
bool ethernetLinkUp()         { return sLinkUp; }
bool ethernetHasIp()          { return sHasIp; }
String ethernetIpString()     { return String(sIpStr); }
String ethernetAdapterName()  { return sAdapterName; }

bool ethernetConsumeDirtyFlag() {
    bool d = sDirty;
    sDirty = false;
    return d;
}
