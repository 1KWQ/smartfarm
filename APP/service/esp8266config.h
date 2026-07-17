#ifndef ESP8266_CONFIG_H__
#define ESP8266_CONFIG_H__

// ====== OneNET 凭据  ======
#define ONENET_PRODUCT_ID       "bo9vLVFhge"
#define ONENET_DEVICE_NAME      "AHT20Test"
#define ONENET_DEVICE_TOKEN     "version=2018-10-31&res=products%2Fbo9vLVFhge%2Fdevices%2FAHT20Test&et=1805693871&method=md5&sign=UAS0VIS1IY0rZx0GAAJoPg%3D%3D"

// ====== WiFi 凭据 ======
#define ESP8266_WIFI_SSID       "nubia"
#define ESP8266_WIFI_PASSWORD   "zxcvbnm123"

// ====== MQTT Broker (OneNET 非SSL) ======
#define ONENET_MQTT_BROKER      "mqtts.heclouds.com"
#define ONENET_MQTT_PORT        1883

#define ESP8266_WIFI_TIMEOUT_MS     15000  // WiFi连接超时(要扫描信道)
#define ESP8266_MQTT_TIMEOUT_MS     10000  // MQTT连接超时(TCP握手)
#define ESP8266_MAX_RECONNECT       5      // 最大重连次数, 0=无限


#endif