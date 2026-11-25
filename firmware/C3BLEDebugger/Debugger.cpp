#include "Debugger.h"

#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>
#include <cJSON.h>
#include <esp32-hal-bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <esp_gatt_common_api.h>

namespace {
constexpr char kName[] = "c3-ble-debugger";
constexpr char kBuild[] = "0002";
constexpr int kBaud = 115200;
constexpr int kLineLimit = 768;
constexpr size_t kScanLimit = 128;
constexpr size_t kWriteLimit = 128;
constexpr uint32_t kGattTimeout = 7000;
constexpr uint16_t kAppId = 0;
constexpr uint16_t kUnknownConnection = UINT16_MAX;

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json object() { return {cJSON_CreateObject(), cJSON_Delete}; }

void printJson(cJSON* value, int32_t id = 0) {
    char* line = cJSON_PrintUnformatted(value);
    if (line) {
        Serial.println(line);
        cJSON_free(line);
    } else {
        Serial.printf("{\"id\":%ld,\"ok\":false,\"error\":\"JSON allocation failed\"}\r\n", long(id));
    }
}

class Reply {
public:
    explicit Reply(const cJSON* id) : id_(cJSON_IsNumber(id) ? id->valueint : 0), root_(object()) {
        if (cJSON_IsNumber(id)) cJSON_AddNumberToObject(root_.get(), "id", id_);
        else cJSON_AddNullToObject(root_.get(), "id");
        cJSON_AddBoolToObject(root_.get(), "ok", true);
    }
    ~Reply() { if (!silent) printJson(root_.get(), id_); }
    bool silent = false;
    cJSON* data() {
        if (!data_) data_ = cJSON_AddObjectToObject(root_.get(), "data");
        return data_;
    }
    void error(const char* message) {
        cJSON_ReplaceItemInObjectCaseSensitive(root_.get(), "ok", cJSON_CreateFalse());
        cJSON_AddStringToObject(root_.get(), "error", message);
    }
    void status(int code) { cJSON_AddNumberToObject(data(), "status", code); }
    bool check(int code, const char* message) {
        if (code == 0) return true;
        status(code);
        error(message);
        return false;
    }
private:
    int32_t id_;
    Json root_;
    cJSON* data_ = nullptr;
};

bool integer(const cJSON* value, int minimum, int maximum) {
    if (!cJSON_IsNumber(value)) return false;
    const double number = value->valuedouble;
    return number >= minimum && number <= maximum && number == value->valueint;
}

const char* getString(const cJSON* request, const char* key) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, key));
}

int hexDigit(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

bool parseAddress(const char* text, esp_bd_addr_t address) {
    if (!text || strlen(text) != 17) return false;
    for (unsigned index = 0; index < sizeof(esp_bd_addr_t); ++index) {
        const int high = hexDigit(text[index * 3]);
        const int low = hexDigit(text[index * 3 + 1]);
        if (high < 0 || low < 0 || (index < 5 && text[index * 3 + 2] != ':')) return false;
        address[index] = (high << 4) | low;
    }
    return true;
}

bool sameAddress(const uint8_t* left, const uint8_t* right) {
    return memcmp(left, right, sizeof(esp_bd_addr_t)) == 0;
}

void addAddress(cJSON* json, const uint8_t* address) {
    char text[18];
    snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
             address[0], address[1], address[2], address[3], address[4], address[5]);
    cJSON_AddStringToObject(json, "address", text);
}

const char* addressType(esp_ble_addr_type_t type) {
    return type == BLE_ADDR_TYPE_RANDOM || type == BLE_ADDR_TYPE_RPA_RANDOM ? "random" : "public";
}

void addHex(cJSON* json, const uint8_t* value, size_t length) {
    constexpr char digits[] = "0123456789abcdef";
    static char hex[2 * ESP_GATT_MAX_ATTR_LEN + 1];
    for (size_t index = 0; index < length; ++index) {
        hex[index * 2] = digits[value[index] >> 4];
        hex[index * 2 + 1] = digits[value[index] & 0x0f];
    }
    hex[length * 2] = '\0';
    cJSON_AddStringToObject(json, "hex", hex);
}

void uuidStr(const esp_bt_uuid_t& uuid, char (&text)[37]) {
    if (uuid.len == ESP_UUID_LEN_16) {
        snprintf(text, sizeof(text), "%04x", uuid.uuid.uuid16);
    } else if (uuid.len == ESP_UUID_LEN_32) {
        snprintf(text, sizeof(text), "%08lx", static_cast<unsigned long>(uuid.uuid.uuid32));
    } else {
        const uint8_t* u = uuid.uuid.uuid128;
        snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8], u[7], u[6], u[5], u[4], u[3], u[2], u[1], u[0]);
    }
}

void addUuid(cJSON* json, const esp_bt_uuid_t& uuid) {
    char text[37];
    uuidStr(uuid, text);
    cJSON_AddStringToObject(json, "uuid", text);
}

// This runs only on the Arduino task. BLE callbacks enqueue copies of data before returning.
enum class Kind {
    Registration, ScanParams, ScanStart, ScanDone, ScanStop, Open, Disconnect, Mtu,
    SearchDone, Read, Write, WriteDescriptor, RegisterNotify, UnregisterNotify,
    Notification, Auth, AuthUnsupported, Forget,
};

struct Event {
    Kind kind;
    int status = 0;
    uint16_t conn = kUnknownConnection;
    uint16_t handle = 0;
    uint16_t length = 0;
    uint32_t ms = 0;
    bool flag = false;
    esp_bd_addr_t address{};
    uint8_t value[ESP_GATT_MAX_ATTR_LEN];
    explicit Event(Kind kind) : kind(kind) {}
    void reset(Kind nextKind) {
        kind = nextKind;
        status = 0;
        conn = kUnknownConnection;
        handle = 0;
        length = 0;
        ms = 0;
        flag = false;
        memset(address, 0, sizeof(address));
    }
};

QueueHandle_t controlQueue = nullptr;
QueueHandle_t notificationQueue = nullptr;
std::atomic<uint32_t> dropped{0};
std::atomic<bool> pairingAllowed{false};

// The callbacks each run on the BLE task. The command waiter and event drain
// run only on loopTask. Keep their payload scratch space off both task stacks.
Event commandResult(Kind::Registration);

struct ScanDevice {
    esp_bd_addr_t address;
    esp_ble_addr_type_t type;
    int rssi;
    char name[ESP_BLE_ADV_DATA_LEN_MAX + 1];
};

portMUX_TYPE scanLock = portMUX_INITIALIZER_UNLOCKED;
ScanDevice devices[kScanLimit];
size_t deviceCount = 0;
bool scanTruncated = false;
bool collectScan = false;

void enqueue(const Event& event) {
    QueueHandle_t queue = event.kind == Kind::Notification ? notificationQueue : controlQueue;
    if (xQueueSend(queue, &event, 0) != pdTRUE) dropped.fetch_add(1, std::memory_order_relaxed);
}

// Local names are untrusted bytes. Keep valid UTF-8 and replace malformed bytes.
void copyName(char* output, const uint8_t* bytes, size_t length) {
    size_t offset = 0;
    while (offset < length && bytes[offset]) {
        const uint8_t first = bytes[offset];
        size_t width = 1;
        if (first >= 0xc2 && first <= 0xdf) width = 2;
        else if (first >= 0xe0 && first <= 0xef) width = 3;
        else if (first >= 0xf0 && first <= 0xf4) width = 4;
        bool valid = first < 0x80 || (width > 1 && offset + width <= length);
        if (valid && width > 1) {
            const uint8_t second = bytes[offset + 1];
            valid = !(first == 0xe0 && second < 0xa0) && !(first == 0xed && second >= 0xa0) &&
                    !(first == 0xf0 && second < 0x90) && !(first == 0xf4 && second >= 0x90);
            for (size_t index = 1; index < width; ++index) valid &= (bytes[offset + index] & 0xc0) == 0x80;
        }
        if (valid) {
            memcpy(output + offset, bytes + offset, width);
            offset += width;
        } else {
            output[offset++] = '?';
        }
    }
    output[offset] = '\0';
}

void recordAdvertisement(const esp_ble_gap_cb_param_t::ble_scan_result_evt_param& report) {
    char name[ESP_BLE_ADV_DATA_LEN_MAX + 1]{};
    size_t length = min(sizeof(report.ble_adv), size_t(report.adv_data_len) + report.scan_rsp_len);
    for (size_t offset = 0; offset < length && report.ble_adv[offset] != 0;) {
        const size_t fieldLength = report.ble_adv[offset];
        if (offset + fieldLength >= length) break;
        uint8_t type = report.ble_adv[offset + 1];
        if (type == ESP_BLE_AD_TYPE_NAME_CMPL || (type == ESP_BLE_AD_TYPE_NAME_SHORT && !name[0])) {
            copyName(name, report.ble_adv + offset + 2, min(fieldLength - 1, sizeof(name) - 1));
        }
        offset += fieldLength + 1;
    }
    portENTER_CRITICAL(&scanLock);
    if (collectScan) {
        size_t index = 0;
        for (; index < deviceCount; ++index) {
            if (devices[index].type == report.ble_addr_type && sameAddress(devices[index].address, report.bda)) break;
        }
        if (index == kScanLimit) {
            scanTruncated = true;
        } else {
            ScanDevice& device = devices[index];
            if (index == deviceCount) {
                ++deviceCount;
                memcpy(device.address, report.bda, sizeof(device.address));
                device.type = report.ble_addr_type;
                device.name[0] = '\0';
            }
            device.rssi = report.rssi;
            if (name[0]) memcpy(device.name, name, sizeof(name));
        }
    }
    portEXIT_CRITICAL(&scanLock);
}

void gapCallback(esp_gap_ble_cb_event_t type, esp_ble_gap_cb_param_t* param) {
    static Event event(Kind::ScanDone);
    event.reset(Kind::ScanDone);
    switch (type) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            event.kind = Kind::ScanParams;
            event.status = param->scan_param_cmpl.status;
            break;
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            event.kind = Kind::ScanStart;
            event.status = param->scan_start_cmpl.status;
            break;
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            event.kind = Kind::ScanStop;
            event.status = param->scan_stop_cmpl.status;
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) recordAdvertisement(param->scan_rst);
            if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_CMPL_EVT) return;
            break;
        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, pairingAllowed.load());
            return;
        case ESP_GAP_BLE_NC_REQ_EVT:
            esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, false);
            event.kind = Kind::AuthUnsupported;
            memcpy(event.address, param->ble_security.key_notif.bd_addr, sizeof(event.address));
            break;
        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, false, 0);
            event.kind = Kind::AuthUnsupported;
            memcpy(event.address, param->ble_security.ble_req.bd_addr, sizeof(event.address));
            break;
        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            event.kind = Kind::AuthUnsupported;
            memcpy(event.address, param->ble_security.key_notif.bd_addr, sizeof(event.address));
            break;
        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            const auto& auth = param->ble_security.auth_cmpl;
            event.kind = Kind::Auth;
            event.flag = auth.success;
            event.status = auth.fail_reason;
            memcpy(event.address, auth.bd_addr, sizeof(event.address));
            break;
        }
        case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
            event.kind = Kind::Forget;
            event.status = param->remove_bond_dev_cmpl.status;
            memcpy(event.address, param->remove_bond_dev_cmpl.bd_addr, sizeof(event.address));
            break;
        default:
            return;
    }
    enqueue(event);
}

void gattCallback(esp_gattc_cb_event_t type, esp_gatt_if_t interfaceId, esp_ble_gattc_cb_param_t* param) {
    static Event event(Kind::Registration);
    event.reset(Kind::Registration);
    switch (type) {
        case ESP_GATTC_REG_EVT:
            if (param->reg.app_id != kAppId) return;
            event.status = param->reg.status;
            event.handle = interfaceId;
            break;
        case ESP_GATTC_OPEN_EVT:
            event.kind = Kind::Open;
            event.status = param->open.status;
            event.conn = param->open.conn_id;
            event.handle = param->open.mtu;
            memcpy(event.address, param->open.remote_bda, sizeof(event.address));
            break;
        case ESP_GATTC_DISCONNECT_EVT:
            event.kind = Kind::Disconnect;
            event.conn = param->disconnect.conn_id;
            event.status = param->disconnect.reason;
            memcpy(event.address, param->disconnect.remote_bda, sizeof(event.address));
            break;
        case ESP_GATTC_CFG_MTU_EVT:
            event.kind = Kind::Mtu;
            event.conn = param->cfg_mtu.conn_id;
            event.status = param->cfg_mtu.status;
            event.handle = param->cfg_mtu.mtu;
            break;
        case ESP_GATTC_SEARCH_CMPL_EVT:
            event.kind = Kind::SearchDone;
            event.conn = param->search_cmpl.conn_id;
            event.status = param->search_cmpl.status;
            break;
        case ESP_GATTC_READ_CHAR_EVT:
        case ESP_GATTC_READ_DESCR_EVT:
            event.kind = Kind::Read;
            event.conn = param->read.conn_id;
            event.handle = param->read.handle;
            event.status = param->read.status;
            if (!event.status && param->read.value && param->read.value_len <= sizeof(event.value)) {
                event.length = param->read.value_len;
                memcpy(event.value, param->read.value, event.length);
            } else if (!event.status && param->read.value_len) {
                event.status = ESP_GATT_INVALID_ATTR_LEN;
            }
            break;
        case ESP_GATTC_WRITE_CHAR_EVT:
        case ESP_GATTC_WRITE_DESCR_EVT:
            event.kind = type == ESP_GATTC_WRITE_CHAR_EVT ? Kind::Write : Kind::WriteDescriptor;
            event.conn = param->write.conn_id;
            event.handle = param->write.handle;
            event.status = param->write.status;
            break;
        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            event.kind = Kind::RegisterNotify;
            event.status = param->reg_for_notify.status;
            event.handle = param->reg_for_notify.handle;
            break;
        case ESP_GATTC_UNREG_FOR_NOTIFY_EVT:
            event.kind = Kind::UnregisterNotify;
            event.status = param->unreg_for_notify.status;
            event.handle = param->unreg_for_notify.handle;
            break;
        case ESP_GATTC_NOTIFY_EVT:
            if (param->notify.value_len > sizeof(event.value) || (param->notify.value_len && !param->notify.value)) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            event.kind = Kind::Notification;
            event.conn = param->notify.conn_id;
            event.handle = param->notify.handle;
            event.length = param->notify.value_len;
            event.ms = millis();
            event.flag = param->notify.is_notify;
            if (event.length) memcpy(event.value, param->notify.value, event.length);
            memcpy(event.address, param->notify.remote_bda, sizeof(event.address));
            break;
        default:
            return;
    }
    enqueue(event);
}

esp_gatt_if_t gattInterface = ESP_GATT_IF_NONE;
esp_err_t initError = ESP_OK;
esp_bd_addr_t peerAddress{};
esp_ble_addr_type_t peerType = BLE_ADDR_TYPE_PUBLIC;
uint16_t connection = kUnknownConnection;
uint16_t mtu = 23;
bool opening = false;
bool abandonOpen = false;
bool closing = false;
bool connectedSecure = false;
bool scanning = false;

struct Characteristic {
    uint16_t handle;
    esp_bt_uuid_t uuid;
    uint16_t ccc;
    esp_gatt_char_prop_t properties;
};
std::vector<Characteristic> characteristics;
std::vector<uint16_t> subscriptions;

const Characteristic* findCharacteristic(uint16_t handle) {
    for (const auto& characteristic : characteristics) {
        if (characteristic.handle == handle) return &characteristic;
    }
    return nullptr;
}

bool connected() { return connection != kUnknownConnection; }

Json eventObject(const char* name) {
    Json data = object();
    cJSON_AddStringToObject(data.get(), "event", name);
    return data;
}

void clearConnection() {
    connection = kUnknownConnection;
    closing = false;
    connectedSecure = false;
    pairingAllowed.store(false);
    mtu = 23;
    characteristics.clear();
    subscriptions.clear();
    xQueueReset(notificationQueue);
}

void handleControl(const Event& event) {
    switch (event.kind) {
        case Kind::Registration:
            if (!event.status) gattInterface = event.handle;
            else initError = ESP_FAIL;
            break;
        case Kind::Open:
            if (!opening || !sameAddress(peerAddress, event.address)) break;
            opening = false;
            if (event.status) break;
            connection = event.conn;
            mtu = max<uint16_t>(23, event.handle);
            if (abandonOpen) {
                closing = esp_ble_gap_disconnect(peerAddress) == ESP_OK;
                break;
            }
            esp_ble_gattc_send_mtu_req(gattInterface, connection);
            {
                Json message = eventObject("connected");
                addAddress(message.get(), peerAddress);
                cJSON_AddStringToObject(message.get(), "address_type", addressType(peerType));
                printJson(message.get());
            }
            break;
        case Kind::Disconnect:
            if (!connected() || connection != event.conn || !sameAddress(peerAddress, event.address)) break;
            clearConnection();
            {
                Json message = eventObject("disconnected");
                addAddress(message.get(), event.address);
                cJSON_AddNumberToObject(message.get(), "reason", event.status);
                printJson(message.get());
            }
            break;
        case Kind::Mtu:
            if (connection == event.conn && !event.status) mtu = event.handle;
            break;
        case Kind::ScanDone:
            scanning = false;
            break;
        case Kind::ScanStop:
            if (!event.status) scanning = false;
            break;
        case Kind::Auth:
        case Kind::AuthUnsupported:
            if (!connected()) break;
            // The SDK can report the bonded identity address instead of an advertising RPA.
            if (!sameAddress(peerAddress, event.address) && !pairingAllowed.load()) break;
            connectedSecure = event.flag;
            {
                Json message = eventObject("auth");
                addAddress(message.get(), event.address);
                cJSON_AddBoolToObject(message.get(), "ok", event.flag);
                cJSON_AddNumberToObject(message.get(), "status", event.status);
                if (event.kind == Kind::AuthUnsupported) {
                    cJSON_AddStringToObject(message.get(), "error", "Peer requires a passkey or numeric confirmation");
                }
                printJson(message.get());
            }
            break;
        default:
            break;
    }
}

bool waitFor(Event& result, Kind expected, uint32_t timeout, uint16_t handle = 0,
             uint16_t conn = kUnknownConnection, const uint8_t* address = nullptr) {
    const uint32_t started = millis();
    while (millis() - started < timeout) {
        static Event event(Kind::Registration);
        if (xQueueReceive(controlQueue, &event, 1) != pdTRUE) continue;
        handleControl(event);
        const bool matches = event.kind == expected && (!handle || event.handle == handle) &&
                             (conn == kUnknownConnection || event.conn == conn) &&
                             (!address || sameAddress(address, event.address));
        const bool auth = expected == Kind::Auth && (event.kind == Kind::Auth || event.kind == Kind::AuthUnsupported) &&
                          connected() && connection == conn;
        if (matches || auth) {
            result = event;
            return true;
        }
        if ((conn != kUnknownConnection || expected == Kind::RegisterNotify || expected == Kind::UnregisterNotify) &&
            !connected()) return false;
    }
    return false;
}

void hello(Reply& reply) {
    cJSON* data = reply.data();
    cJSON_AddStringToObject(data, "name", kName);
    cJSON_AddStringToObject(data, "build", kBuild);
    cJSON_AddNumberToObject(data, "protocol", 1);
    cJSON_AddNumberToObject(data, "baud", kBaud);
}

void connectionData(cJSON* data) {
    cJSON_AddBoolToObject(data, "connected", connected());
    cJSON_AddBoolToObject(data, "secure", connectedSecure);
    if (connected()) {
        addAddress(data, peerAddress);
        cJSON_AddStringToObject(data, "address_type", addressType(peerType));
        cJSON_AddNumberToObject(data, "mtu", mtu);
    }
}

void scan(const cJSON* request, Reply& reply) {
    const cJSON* seconds = cJSON_GetObjectItemCaseSensitive(request, "seconds");
    if (!integer(seconds, 1, 15)) return reply.error("seconds must be an integer from 1 to 15");
    if (scanning || connected() || opening) return reply.error("Disconnect or wait for the current scan before scanning");
    portENTER_CRITICAL(&scanLock);
    deviceCount = 0;
    scanTruncated = false;
    collectScan = true;
    portEXIT_CRITICAL(&scanLock);
    const esp_err_t started = esp_ble_gap_start_scanning(seconds->valueint);
    scanning = started == ESP_OK;
    Event& result = commandResult;
    bool complete = reply.check(started, "Scan request failed");
    if (complete && !waitFor(result, Kind::ScanStart, 2000)) {
        reply.error("Scan start timed out");
        complete = false;
    }
    if (complete && result.status) {
        scanning = false;
        complete = reply.check(result.status, "Scan could not start");
    }
    if (complete && !waitFor(result, Kind::ScanDone, seconds->valueint * 1000 + 3000)) {
        reply.error("Scan timed out");
        complete = false;
    }
    portENTER_CRITICAL(&scanLock);
    collectScan = false;
    portEXIT_CRITICAL(&scanLock);
    if (!complete) {
        if (scanning) esp_ble_gap_stop_scanning();
        return;
    }
    cJSON* data = reply.data();
    cJSON* list = cJSON_AddArrayToObject(data, "devices");
    for (size_t index = 0; index < deviceCount; ++index) {
        const ScanDevice& device = devices[index];
        cJSON* item = cJSON_CreateObject();
        cJSON_AddItemToArray(list, item);
        addAddress(item, device.address);
        cJSON_AddStringToObject(item, "address_type", addressType(device.type));
        cJSON_AddStringToObject(item, "name", device.name);
        cJSON_AddNumberToObject(item, "rssi", device.rssi);
    }
    cJSON_AddBoolToObject(data, "truncated", scanTruncated);
}

void closeLink() {
    if (connected() && !closing) closing = esp_ble_gap_disconnect(peerAddress) == ESP_OK;
}

bool waitGatt(Reply& reply, Event& result, Kind kind, uint16_t handle = 0,
              uint16_t conn = kUnknownConnection) {
    if (waitFor(result, kind, kGattTimeout, handle, conn)) return true;
    if (connected()) {
        // Retire the link so a late completion cannot satisfy a newer request.
        closeLink();
        reply.error("GATT operation timed out. Disconnect to reset the link before retrying");
    } else {
        reply.error("Peer disconnected during GATT operation");
    }
    return false;
}

bool pair(Reply& reply) {
    if (connectedSecure) return true;
    pairingAllowed.store(true);
    const esp_err_t status = esp_ble_set_encryption(peerAddress, ESP_BLE_SEC_ENCRYPT);
    Event& result = commandResult;
    const bool queued = reply.check(status, "Could not request BLE security");
    const bool completed = queued && waitFor(result, Kind::Auth, 14000, 0, connection);
    pairingAllowed.store(false);
    if (!queued) return false;
    if (!completed) {
        reply.error(connected() ? "Pairing timed out. This debugger only supports Just Works" : "Peer disconnected during pairing");
        closeLink();
        return false;
    }
    if (!result.flag) {
        reply.status(result.status);
        reply.error("Pairing failed. This debugger only supports Just Works without a passkey or PIN");
        closeLink();
        return false;
    }
    return true;
}

void connect(const cJSON* request, Reply& reply) {
    if (connected() || opening || scanning) return reply.error("Disconnect or wait for the current scan or connection");
    esp_bd_addr_t address;
    if (!parseAddress(getString(request, "address"), address)) return reply.error("address must be a Bluetooth MAC address");
    const char* type = getString(request, "address_type");
    if (!type || (strcmp(type, "public") && strcmp(type, "random"))) return reply.error("address_type must be public or random");
    const cJSON* secure = cJSON_GetObjectItemCaseSensitive(request, "secure");
    if (secure && !cJSON_IsBool(secure)) return reply.error("secure must be a boolean");
    memcpy(peerAddress, address, sizeof(address));
    peerType = !strcmp(type, "random") ? BLE_ADDR_TYPE_RANDOM : BLE_ADDR_TYPE_PUBLIC;
    abandonOpen = false;
    const esp_err_t status = esp_ble_gattc_open(gattInterface, peerAddress, peerType, true);
    if (!reply.check(status, "Connection request failed")) return;
    opening = true;
    Event& result = commandResult;
    if (!waitFor(result, Kind::Open, 14000, 0, kUnknownConnection, address)) {
        abandonOpen = true;
        return reply.error("Connection timed out. Wait for the Bluetooth controller to finish before retrying");
    }
    if (!reply.check(result.status, "GATT connection failed")) return;
    if (cJSON_IsTrue(secure) && !pair(reply)) return;
    connectionData(reply.data());
}

void disconnect(Reply& reply) {
    if (opening) {
        abandonOpen = true;
        return reply.error("Connection attempt is still pending. Any late connection will be closed");
    }
    if (connected()) {
        Event& result = commandResult;
        if (!closing && !reply.check(esp_ble_gap_disconnect(peerAddress), "Disconnect request failed")) return;
        closing = true;
        if (!waitFor(result, Kind::Disconnect, 4500, 0, connection)) return reply.error("Disconnect timed out");
    }
    connectionData(reply.data());
}

bool endOfCache(esp_gatt_status_t status, uint16_t count) {
    return status == ESP_GATT_INVALID_OFFSET || status == ESP_GATT_NOT_FOUND || (status == ESP_GATT_OK && !count);
}

void discover(Reply& reply) {
    const esp_err_t requested = esp_ble_gattc_search_service(gattInterface, connection, nullptr);
    if (!reply.check(requested, "Service discovery request failed")) return;
    Event& result = commandResult;
    if (!waitGatt(reply, result, Kind::SearchDone, 0, connection) ||
        !reply.check(result.status, "GATT service discovery failed")) return;

    constexpr size_t maxAttributes = 256;
    size_t attributes = 0;
    std::vector<Characteristic> discovered;
    cJSON* list = cJSON_AddArrayToObject(reply.data(), "services");
    for (uint16_t serviceOffset = 0;; ++serviceOffset) {
        esp_gattc_service_elem_t service;
        uint16_t count = 1;
        esp_gatt_status_t status = esp_ble_gattc_get_service(gattInterface, connection, nullptr, &service, &count, serviceOffset);
        if (endOfCache(status, count)) break;
        if (!reply.check(status, "Could not enumerate services")) return;
        if (++attributes > maxAttributes) return reply.error("GATT database exceeds 256 entries");
        cJSON* serviceJson = cJSON_CreateObject();
        cJSON_AddItemToArray(list, serviceJson);
        addUuid(serviceJson, service.uuid);
        cJSON_AddNumberToObject(serviceJson, "start_handle", service.start_handle);
        cJSON_AddNumberToObject(serviceJson, "end_handle", service.end_handle);
        cJSON* charList = cJSON_AddArrayToObject(serviceJson, "characteristics");
        for (uint16_t charOffset = 0;; ++charOffset) {
            esp_gattc_char_elem_t entry;
            count = 1;
            status = esp_ble_gattc_get_all_char(gattInterface, connection, service.start_handle, service.end_handle,
                                              &entry, &count, charOffset);
            if (endOfCache(status, count)) break;
            if (!reply.check(status, "Could not enumerate characteristics")) return;
            if (++attributes > maxAttributes) return reply.error("GATT database exceeds 256 entries");
            cJSON* item = cJSON_CreateObject();
            cJSON_AddItemToArray(charList, item);
            addUuid(item, entry.uuid);
            cJSON_AddNumberToObject(item, "handle", entry.char_handle);
            cJSON_AddBoolToObject(item, "read", entry.properties & ESP_GATT_CHAR_PROP_BIT_READ);
            cJSON_AddBoolToObject(item, "write", entry.properties & ESP_GATT_CHAR_PROP_BIT_WRITE);
            cJSON_AddBoolToObject(item, "write_without_response", entry.properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR);
            cJSON_AddBoolToObject(item, "notify", entry.properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY);
            cJSON_AddBoolToObject(item, "indicate", entry.properties & ESP_GATT_CHAR_PROP_BIT_INDICATE);
            Characteristic characteristic{entry.char_handle, entry.uuid, 0, entry.properties};
            cJSON* descList = cJSON_AddArrayToObject(item, "descriptors");
            for (uint16_t descOffset = 0;; ++descOffset) {
                esp_gattc_descr_elem_t descriptor;
                count = 1;
                status = esp_ble_gattc_get_all_descr(gattInterface, connection, entry.char_handle, &descriptor, &count, descOffset);
                if (endOfCache(status, count)) break;
                if (!reply.check(status, "Could not enumerate descriptors")) return;
                if (++attributes > maxAttributes) return reply.error("GATT database exceeds 256 entries");
                cJSON* desc = cJSON_CreateObject();
                cJSON_AddItemToArray(descList, desc);
                addUuid(desc, descriptor.uuid);
                cJSON_AddNumberToObject(desc, "handle", descriptor.handle);
                if (descriptor.uuid.len == ESP_UUID_LEN_16 && descriptor.uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
                    characteristic.ccc = descriptor.handle;
                }
            }
            discovered.push_back(characteristic);
        }
    }
    characteristics.swap(discovered);
}

void readAttribute(uint16_t handle, Reply& reply) {
    const esp_err_t status = esp_ble_gattc_read_char(gattInterface, connection, handle, ESP_GATT_AUTH_REQ_NONE);
    if (!reply.check(status, "GATT read request rejected")) return;
    Event& result = commandResult;
    if (!waitGatt(reply, result, Kind::Read, handle, connection) || !reply.check(result.status, "GATT read failed")) return;
    reply.status(result.status);
    addHex(reply.data(), result.value, result.length);
}

void writeAttribute(const cJSON* request, uint16_t handle, Reply& reply) {
    const char* hex = getString(request, "hex");
    const size_t length = hex ? strlen(hex) : 0;
    if (!length || length % 2 || length > kWriteLimit * 2) return reply.error("hex must contain 1 to 128 bytes in hexadecimal");
    uint8_t bytes[kWriteLimit];
    for (size_t index = 0; index < length / 2; ++index) {
        const int high = hexDigit(hex[2 * index]);
        const int low = hexDigit(hex[2 * index + 1]);
        if (high < 0 || low < 0) return reply.error("hex must contain only hexadecimal digits");
        bytes[index] = (high << 4) | low;
    }
    const cJSON* response = cJSON_GetObjectItemCaseSensitive(request, "response");
    if (response && !cJSON_IsBool(response)) return reply.error("response must be a boolean");
    const bool confirmed = !cJSON_IsFalse(response);
    if (length / 2 > mtu - 3) return reply.error("Payload exceeds the negotiated ATT MTU minus 3 bytes");
    const esp_err_t status = esp_ble_gattc_write_char(gattInterface, connection, handle, length / 2, bytes,
                                                    confirmed ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
                                                    ESP_GATT_AUTH_REQ_NONE);
    if (!reply.check(status, "GATT write request rejected")) return;
    Event& result = commandResult;
    if (!waitGatt(reply, result, Kind::Write, handle, connection) || !reply.check(result.status, "GATT write failed")) return;
    reply.status(result.status);
    cJSON_AddBoolToObject(reply.data(), "confirmed", confirmed);
}

bool registerNotifications(uint16_t handle, bool enable, Reply& reply) {
    const esp_err_t status = enable ? esp_ble_gattc_register_for_notify(gattInterface, peerAddress, handle) :
                                      esp_ble_gattc_unregister_for_notify(gattInterface, peerAddress, handle);
    if (!reply.check(status, "Local notification registration request failed")) return false;
    const Kind kind = enable ? Kind::RegisterNotify : Kind::UnregisterNotify;
    Event& result = commandResult;
    return waitGatt(reply, result, kind, handle) && reply.check(result.status, "Local notification registration failed");
}

void subscription(const cJSON* request, uint16_t handle, bool enable, Reply& reply) {
    const Characteristic* characteristic = findCharacteristic(handle);
    if (!characteristic) return reply.error("Run discover and select a characteristic handle from this connection");
    const uint16_t ccc = characteristic->ccc;
    if (!ccc) return reply.error("Characteristic has no Client Characteristic Configuration descriptor");
    const char* mode = enable ? getString(request, "mode") : nullptr;
    uint8_t bits = 0;
    if (enable) {
        if (!mode || (strcmp(mode, "notify") && strcmp(mode, "indicate"))) return reply.error("mode must be notify or indicate");
        bits = !strcmp(mode, "notify") ? 1 : 2;
        const auto required = bits == 1 ? ESP_GATT_CHAR_PROP_BIT_NOTIFY : ESP_GATT_CHAR_PROP_BIT_INDICATE;
        if (!(characteristic->properties & required)) return reply.error("Requested mode is unsupported by the characteristic");
    }
    auto previous = std::find(subscriptions.begin(), subscriptions.end(), handle);
    const bool registered = previous != subscriptions.end();
    const bool needsRegistration = enable && !registered;
    if (needsRegistration && !registerNotifications(handle, true, reply)) return;
    uint8_t value[] = {bits, 0};
    const esp_err_t status = esp_ble_gattc_write_char_descr(gattInterface, connection, ccc, sizeof(value), value,
                                                          ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    Event& result = commandResult;
    const bool success = reply.check(status, "CCCD write request failed") &&
                         waitGatt(reply, result, Kind::WriteDescriptor, ccc, connection) &&
                         reply.check(result.status, "Peer rejected CCCD write");
    if (!success) {
        if (needsRegistration && connected() && !closing) {
            Reply rollback(nullptr);
            rollback.silent = true;
            if (!registerNotifications(handle, false, rollback)) closeLink();
        }
        return;
    }
    if (needsRegistration) subscriptions.push_back(handle);
    if (!enable && registered) {
        if (!registerNotifications(handle, false, reply)) {
            closeLink();
            return;
        }
        subscriptions.erase(previous);
    }
    reply.status(0);
    cJSON_AddBoolToObject(reply.data(), "subscribed", enable);
    if (enable) cJSON_AddStringToObject(reply.data(), "mode", mode);
}

bool getBonds(std::vector<esp_ble_bond_dev_t>& records, Reply& reply) {
    int count = esp_ble_get_bond_device_num();
    if (count < 0) {
        reply.error("Could not read the BLE bond table");
        return false;
    }
    if (!count) return true;
    records.resize(count);
    if (!reply.check(esp_ble_get_bond_device_list(&count, records.data()), "Could not read BLE bonds")) return false;
    records.resize(count);
    return true;
}

void bonds(Reply& reply) {
    std::vector<esp_ble_bond_dev_t> records;
    if (!getBonds(records, reply)) return;
    cJSON* list = cJSON_AddArrayToObject(reply.data(), "bonds");
    for (const auto& record : records) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddItemToArray(list, item);
        addAddress(item, record.bd_addr);
    }
}

void forget(const cJSON* request, Reply& reply) {
    if (connected() || opening) return reply.error("Disconnect before removing a saved bond");
    esp_bd_addr_t address;
    if (!parseAddress(getString(request, "address"), address)) return reply.error("address must be a Bluetooth MAC address");
    std::vector<esp_ble_bond_dev_t> records;
    if (!getBonds(records, reply)) return;
    const auto found = std::find_if(records.begin(), records.end(), [&](const esp_ble_bond_dev_t& record) {
        return sameAddress(address, record.bd_addr);
    });
    if (found == records.end()) return reply.error("No saved bond for this address");
    if (!reply.check(esp_ble_remove_bond_device(address), "Bond removal request failed")) return;
    Event& result = commandResult;
    if (!waitFor(result, Kind::Forget, 4500, 0, kUnknownConnection, address)) return reply.error("Bond removal timed out. Run bonds to check the result");
    if (!reply.check(result.status, "BLE stack could not remove the saved bond")) return;
    addAddress(reply.data(), address);
    cJSON_AddBoolToObject(reply.data(), "removed", true);
}

void emitNotifications() {
    static Event item(Kind::Notification);
    for (unsigned count = 0; count < 4 && xQueueReceive(notificationQueue, &item, 0) == pdTRUE; ++count) {
        if (!connected() || item.conn != connection || !sameAddress(peerAddress, item.address)) continue;
        if (std::find(subscriptions.begin(), subscriptions.end(), item.handle) == subscriptions.end()) continue;
        const Characteristic* characteristic = findCharacteristic(item.handle);
        if (!characteristic) continue;
        Json message = eventObject("notification");
        addAddress(message.get(), item.address);
        cJSON_AddNumberToObject(message.get(), "handle", item.handle);
        cJSON_AddNumberToObject(message.get(), "ms", item.ms);
        cJSON_AddStringToObject(message.get(), "mode", item.flag ? "notify" : "indicate");
        addUuid(message.get(), characteristic->uuid);
        addHex(message.get(), item.value, item.length);
        printJson(message.get());
    }
}

void dispatch(const char* line) {
    Json request(cJSON_ParseWithOpts(line, nullptr, true), cJSON_Delete);
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(request.get(), "id");
    bool validId = integer(id, INT32_MIN, INT32_MAX);
    Reply reply(validId ? id : nullptr);
    if (!cJSON_IsObject(request.get())) return reply.error("Expected a JSON object");
    if (!validId) return reply.error("id must be a signed 32-bit integer");
    const char* command = getString(request.get(), "cmd");
    if (!command || !*command) return reply.error("cmd must be a nonempty string");
    if (!strcmp(command, "hello")) return hello(reply);
    if (!strcmp(command, "status")) {
        cJSON* data = reply.data();
        cJSON_AddBoolToObject(data, "ready", initError == ESP_OK && gattInterface != ESP_GATT_IF_NONE);
        cJSON_AddNumberToObject(data, "init_status", initError);
        cJSON_AddNumberToObject(data, "dropped", dropped.load());
        return connectionData(data);
    }
    if (initError != ESP_OK || gattInterface == ESP_GATT_IF_NONE) return reply.error("BLE controller is not ready");
    if (!strcmp(command, "scan")) return scan(request.get(), reply);
    if (!strcmp(command, "connect")) return connect(request.get(), reply);
    if (!strcmp(command, "disconnect")) return disconnect(reply);
    if (!strcmp(command, "bonds")) return bonds(reply);
    if (!strcmp(command, "forget")) return forget(request.get(), reply);
    const bool read = !strcmp(command, "read");
    const bool write = !strcmp(command, "write");
    const bool subscribe = !strcmp(command, "subscribe");
    const bool unsubscribe = !strcmp(command, "unsubscribe");
    const bool pairing = !strcmp(command, "pair");
    const bool discovery = !strcmp(command, "discover");
    if (!read && !write && !subscribe && !unsubscribe && !pairing && !discovery) return reply.error("Unknown command");
    if (!connected() || closing) return reply.error("Connect to a peer first");
    if (pairing) {
        if (pair(reply)) connectionData(reply.data());
        return;
    }
    if (discovery) return discover(reply);
    const cJSON* handle = cJSON_GetObjectItemCaseSensitive(request.get(), "handle");
    if (!integer(handle, 1, UINT16_MAX)) return reply.error("handle must be an integer from 1 to 65535");
    if (read) return readAttribute(handle->valueint, reply);
    if (write) return writeAttribute(request.get(), handle->valueint, reply);
    subscription(request.get(), handle->valueint, subscribe, reply);
}

esp_err_t initialize() {
    controlQueue = xQueueCreate(16, sizeof(Event));
    notificationQueue = xQueueCreate(8, sizeof(Event));
    if (!controlQueue || !notificationQueue) return ESP_ERR_NO_MEM;
    if (!btStart()) return ESP_FAIL;
    esp_err_t status = esp_bluedroid_init();
    if (status != ESP_OK) return status;
    status = esp_bluedroid_enable();
    if (status != ESP_OK) return status;
    status = esp_ble_gap_register_callback(gapCallback);
    if (status != ESP_OK) return status;
    status = esp_ble_gattc_register_callback(gattCallback);
    if (status != ESP_OK) return status;
    uint8_t capability = ESP_IO_CAP_NONE;
    uint8_t auth = ESP_LE_AUTH_BOND;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    status = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &capability, sizeof(capability));
    if (status != ESP_OK) return status;
    status = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    if (status != ESP_OK) return status;
    status = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys));
    if (status != ESP_OK) return status;
    status = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys));
    if (status != ESP_OK) return status;
    status = esp_ble_gatt_set_local_mtu(517);
    if (status != ESP_OK) return status;
    status = esp_ble_gattc_app_register(kAppId);
    if (status != ESP_OK) return status;
    esp_ble_scan_params_t params{};
    params.scan_type = BLE_SCAN_TYPE_ACTIVE;
    params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    params.scan_interval = 0x50;
    params.scan_window = 0x30;
    params.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
    status = esp_ble_gap_set_scan_params(&params);
    if (status != ESP_OK) return status;
    if (!waitFor(commandResult, Kind::ScanParams, 3000)) return ESP_ERR_TIMEOUT;
    return commandResult.status == ESP_BT_STATUS_SUCCESS ? ESP_OK : ESP_FAIL;
}

}  // namespace

void setupDebugger() {
    Serial.setRxBufferSize(2048);
    Serial.begin(kBaud);
    initError = initialize();
}

void runDebugger() {
    static char line[kLineLimit + 1];
    static size_t length = 0;
    static const char* invalidLine = nullptr;
    static Event event(Kind::Open);
    if (controlQueue) {
        while (xQueueReceive(controlQueue, &event, 0) == pdTRUE) handleControl(event);
        if (notificationQueue) emitNotifications();
        static uint32_t reportedDrops = 0;
        const uint32_t currentDrops = dropped.load();
        if (currentDrops != reportedDrops) {
            Json message = eventObject("dropped");
            cJSON_AddNumberToObject(message.get(), "count", currentDrops - reportedDrops);
            cJSON_AddNumberToObject(message.get(), "total", currentDrops);
            printJson(message.get());
            reportedDrops = currentDrops;
        }
    }
    for (unsigned count = 0; count < 128 && Serial.available(); ++count) {
        const int value = Serial.read();
        if (value == '\n') {
            if (invalidLine) {
                Reply reply(nullptr);
                reply.error(invalidLine);
            } else if (length) {
                line[length] = '\0';
                dispatch(line);
            }
            length = 0;
            invalidLine = nullptr;
            break;
        }
        if (value < 0 || invalidLine) continue;
        if (value == 0) invalidLine = "NUL byte in command line";
        else if (length == kLineLimit) invalidLine = "Command line exceeds 768 bytes";
        else line[length++] = static_cast<char>(value);
    }
    delay(1);
}
