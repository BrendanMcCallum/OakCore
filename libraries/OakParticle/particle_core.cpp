#include "particle_core.h"

char OAK_SYSTEM_VERSION_STRING[5] = {"1.00"};
#define PROTOCOL_BUFFER_SIZE 800
#define QUEUE_SIZE 800
unsigned char queue[PROTOCOL_BUFFER_SIZE];

typedef unsigned short uint16_t;
typedef uint16_t chunk_index_t;

#include <Arduino.h>
#include "../ESP8266WiFi/src/ESP8266WiFi.h"
#include "handshake.h"
#include "messages.h"
#include "coap.h"
#include "aes.h"
#include "rsa.h"
#include "dsakeygen.h"
#include "append_list.h"
#include "appender.h"
#include "file_transfer.h"
#include "crc32.h"

WiFiClient pClient; 

#ifdef __cplusplus
extern "C" {
#endif
  #include <c_types.h>
  #include <user_interface.h>
  #include <mem.h>
  #include <osapi.h>
  #include "espmissingincludes.h"
  #include "oakboot.h"
#ifdef __cplusplus
}
#endif


 typedef struct {
  //can cut off here if needed
  char device_id[25];     //device id in hex
  char claim_code[65];   // server public key
  uint8 claimed;   // server public key
  uint8 device_private_key[1216];  // device private key
  uint8 device_public_key[384];   // device public key
  uint8 server_public_key[768]; //also contains the server address at offset 384
  uint8 server_address_type;   //domain or ip of cloud server
  uint8 server_address_length;   //domain or ip of cloud server
  char server_address_domain[254];   //domain or ip of cloud server
  uint8 ota_success;
  uint32 server_address_ip;   //[4]//domain or ip of cloud server
  unsigned short firmware_version;  
  unsigned short system_version;     //
  char version_string[33];    //
  uint8 reserved_flags[32];    //
  uint8 reserved1[32];
  uint8 product_store[24];    
  char ssid[33]; //ssid and terminator
  char passcode[65]; //passcode and terminator
  uint8 channel; //channel number
  int32 third_party_id;    //
  char third_party_data[256];     //
  char first_update_domain[65];
  char first_update_url[65];
  char first_update_fingerprint[60];
  uint8 current_rom_scheme[1];
  uint8 padding2[1];
  uint8 magic;
  uint8 chksum; 
  //uint8 reserved2[698]; 
} oak_config; 

struct msg {
        uint8_t token;
        size_t len;
        uint8_t* response;
        size_t response_len;
    };


#define SPARK_SERVER_PORT 5683


#define USER_VAR_MAX_COUNT            10
#define USER_VAR_KEY_LENGTH           12

#define USER_FUNC_MAX_COUNT           4
#define USER_FUNC_KEY_LENGTH            12
#define USER_FUNC_ARG_LENGTH            64

#define USER_EVENT_NAME_LENGTH            64
#define USER_EVENT_DATA_LENGTH            64

#define SECTOR_SIZE 0x1000
#define DEVICE_CONFIG_SECTOR 256
#define DEVICE_BACKUP_CONFIG_SECTOR 512
#define DEVICE_CHKSUM_INIT 0xee
#define DEVICE_MAGIC 0xf0
#define DEVICE_CONFIG_SIZE 3398
#define PRIVATE_KEY_LENGTH    (612)
#define PUBLIC_KEY_LENGTH    (162)
/* Length in bytes of DER-encoded 2048-bit RSA public key */
#define SERVER_PUBLIC_KEY_LENGTH   (294)
#define SERVER_DOMAIN_LENGTH   (253)


const CloudVariableTypeBool BOOLEAN;
const CloudVariableTypeInt INT;
const CloudVariableTypeString STRING;
const CloudVariableTypeDouble DOUBLE;

uint8 config_buffer[DEVICE_CONFIG_SIZE];
oak_config *deviceConfig = (oak_config*)config_buffer;
uint8 boot_buffer[BOOT_CONFIG_SIZE];
oakboot_config *bootConfig = (oakboot_config*)boot_buffer;


byte device_id[12];

bool spark_initialized = false;


void ERROR(String out){
	Serial.println(out);
}

void INFO(String out){
	Serial.println(out);
}

aes_context aes;

unsigned char key[16];
unsigned char iv_send[16];
unsigned char iv_receive[16];
unsigned char salt[8];
unsigned short _message_id;
unsigned char _token;
uint32_t last_message_millis;
uint32_t last_chunk_millis;    // NB: also used to synchronize time
unsigned short chunk_index;
unsigned short chunk_size;
bool expecting_ping_ack;
bool initialized;
uint8_t updating;






struct User_Var_Lookup_Table_t
{
    const void *userVar;
    Spark_Data_TypeDef userVarType;
    char userVarKey[USER_VAR_KEY_LENGTH+1];

    const void* (*update)(const char* name, Spark_Data_TypeDef varType, const void* var, void* reserved);
};


struct User_Func_Lookup_Table_t
{
    void* pUserFuncData;
    cloud_function_t pUserFunc;
    char userFuncKey[USER_FUNC_KEY_LENGTH];
};



User_Var_Lookup_Table_t* find_var_by_key_or_add(const char* varKey);
User_Func_Lookup_Table_t* find_func_by_key_or_add(const char* funcKey);



static append_list<User_Var_Lookup_Table_t> vars(5);
static append_list<User_Func_Lookup_Table_t> funcs(5);
FilteringEventHandler event_handlers[5];  


User_Var_Lookup_Table_t* find_var_by_key(const char* varKey)
{
    for (int i = vars.size(); i-->0; )
    {
        if (0 == strncmp(vars[i].userVarKey, varKey, USER_VAR_KEY_LENGTH))
        {
            return &vars[i];
        }
    }
    return NULL;
}


User_Var_Lookup_Table_t* find_var_by_key_or_add(const char* varKey)
{
    User_Var_Lookup_Table_t* result = find_var_by_key(varKey);
    return result ? result : vars.add();
}

User_Func_Lookup_Table_t* find_func_by_key(const char* funcKey)
{
    for (int i = funcs.size(); i-->0; )
    {
        if (0 == strncmp(funcs[i].userFuncKey, funcKey, USER_FUNC_KEY_LENGTH))
        {
            return &funcs[i];
        }
    }
    return NULL;
}

User_Func_Lookup_Table_t* find_func_by_key_or_add(const char* funcKey)
{
    User_Func_Lookup_Table_t* result = find_func_by_key(funcKey);
    return result ? result : funcs.add();
}

int call_raw_user_function(void* data, const char* param, void* reserved)
{
    user_function_int_str_t* fn = (user_function_int_str_t*)(data);
    String p(param);
    return (*fn)(p);
}

int call_std_user_function(void* data, const char* param, void* reserved)
{
    user_std_function_int_str_t* fn = (user_std_function_int_str_t*)(data);
    return (*fn)(String(param));
}

void call_wiring_event_handler(const void* handler_data, const char *event_name, const char *data)
{
    wiring_event_handler_t* fn = (wiring_event_handler_t*)(handler_data);
    (*fn)(event_name, data);
}



bool spark_connected()
{
    return pClient.connected();

}

unsigned short next_message_id()
{
  return ++_message_id;
}


static uint8 calc_device_chksum(uint8 *start, uint8 *end) {
  uint8 chksum = DEVICE_CHKSUM_INIT;
  while(start < end) {
    chksum ^= *start;
    start++;
  }
  return chksum;
}

void writeDeviceConfig(){
    deviceConfig->chksum = calc_device_chksum((uint8*)deviceConfig,(uint8*)&deviceConfig->chksum);
    noInterrupts();
    spi_flash_erase_sector(DEVICE_CONFIG_SECTOR);
    spi_flash_write(DEVICE_CONFIG_SECTOR * SECTOR_SIZE, reinterpret_cast<uint32_t*>(config_buffer), DEVICE_CONFIG_SIZE);
    spi_flash_erase_sector(DEVICE_BACKUP_CONFIG_SECTOR);
    spi_flash_write(DEVICE_BACKUP_CONFIG_SECTOR * SECTOR_SIZE, reinterpret_cast<uint32_t*>(config_buffer), DEVICE_CONFIG_SIZE);
    interrupts();
    
  }

uint8_t hex_nibble(unsigned char c) {
    if (c<'0')
        return 0;
    if (c<='9')
        return c-'0';
    if (c<='Z')
        return c-'A'+10;
    if (c<='z')
        return c-'a'+10;
    return 0;
}

size_t hex_decode(uint8_t* buf, size_t len, const char* hex) {
    unsigned char c = '0'; // any non-null character
    size_t i;
    for (i=0; i<len && c; i++) {
        uint8_t b;
        if (!(c = *hex++))
            break;
        b = hex_nibble(c)<<4;
        if (c) {
            c = *hex++;
            b |= hex_nibble(c);
        }
        *buf++ = b;
    }
    return i;
}

// Returns bytes received or -1 on error
int blocking_send(const unsigned char *buf, int length)
{
  if(!spark_connected())
    return -1;

  Serial.println("BLSEND");
	pClient.setTimeout(2000);
	int byte_count = pClient.write(buf, length);
	if(byte_count==0) 
		byte_count = -1;
	return byte_count;
}

// Returns bytes received or -1 on error
int receive(unsigned char *buf, int length)
{

	pClient.setTimeout(2000);
	int available = pClient.available();
	if(available >= length){
		return pClient.readBytes(buf, length);
	}
	else if(available > 0){
		return pClient.readBytes(buf, available);
	}
	else{
		if(!spark_connected())
			return -1;
		else
			return 0;
	}
}

// Returns bytes received or -1 on error
int blocking_receive(unsigned char *buf, int length)
{
  if(!spark_connected())
    return -1;
  Serial.println("BLRECV");
	pClient.setTimeout(2000);
	int byte_count = pClient.readBytes(buf, length);
	if(byte_count==0) 
		byte_count = -1;
	return byte_count;
}


int set_key(const unsigned char *signed_encrypted_credentials)
{
  unsigned char credentials[40];
  unsigned char hmac[20];

  if (0 != decipher_aes_credentials(deviceConfig->device_private_key,
                                    signed_encrypted_credentials,
                                    credentials))
    return 1;//decrypt error

  calculate_ciphertext_hmac(signed_encrypted_credentials, credentials, hmac);

  if (0 == verify_signature(signed_encrypted_credentials + 128,
                            deviceConfig->server_public_key,
                            hmac))
  {
    memcpy(key,        credentials,      16);
    memcpy(iv_send,    credentials + 16, 16);
    memcpy(iv_receive, credentials + 16, 16);
    memcpy(salt,       credentials + 32,  8);
    _message_id = *(credentials + 32) << 8 | *(credentials + 33);
    _token = *(credentials + 34);

    unsigned int seed;
    memcpy(&seed, credentials + 35, 4);

    randomSeed(seed);

    return 0;
  }
  else return 1;//auth error
}

void encrypt(unsigned char *buf, int length)
{
  aes_setkey_enc(&aes, key, 128);
  aes_crypt_cbc(&aes, AES_ENCRYPT, length, iv_send, buf, buf);
  memcpy(iv_send, buf, 16);
}

void ping(unsigned char *buf)
{
  unsigned short message_id = next_message_id();

  buf[0] = 0x40; // Confirmable, no token
  buf[1] = 0x00; // code signifying empty message
  buf[2] = message_id >> 8;
  buf[3] = message_id & 0xff;

  memset(buf + 4, 12, 12); // PKCS #7 padding

  encrypt(buf, 16);
}

size_t wrap(unsigned char *buf, size_t msglen)
{
  size_t buflen = (msglen & ~15) + 16;
  char pad = buflen - msglen;
  memset(buf + 2 + msglen, pad, pad); // PKCS #7 padding

  encrypt(buf + 2, buflen);

  buf[0] = (buflen >> 8) & 0xff;
  buf[1] = buflen & 0xff;

  return buflen + 2;
}


void hello(unsigned char *buf, bool newly_upgraded)
{
  unsigned short message_id = next_message_id();
  size_t len = Messages::hello(buf+2, message_id, newly_upgraded, PLATFORM_ID, PRODUCT_ID, deviceConfig->firmware_version, false, nullptr, 0);
  wrap(buf, len);
}




inline void coded_ack(unsigned char *buf,
                                     unsigned char code,
                                     unsigned char message_id_msb,
                                     unsigned char message_id_lsb
                                     )
{
  buf[0] = 0x60; // acknowledgment, no token
  buf[1] = code;
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;

  memset(buf + 4, 12, 12); // PKCS #7 padding

  encrypt(buf, 16);
}

inline void coded_ack(unsigned char *buf,
                                     unsigned char token,
                                     unsigned char code,
                                     unsigned char message_id_msb,
                                     unsigned char message_id_lsb)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = code;
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;

  memset(buf + 5, 11, 11); // PKCS #7 padding

  encrypt(buf, 16);
}


void variable_value(unsigned char *buf,
                                   unsigned char token,
                                   unsigned char message_id_msb,
                                   unsigned char message_id_lsb,
                                   bool return_value)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = 0x45; // response code 2.05 CONTENT
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;
  buf[5] = 0xff; // payload marker
  buf[6] = return_value ? 1 : 0;

  memset(buf + 7, 9, 9); // PKCS #7 padding

  encrypt(buf, 16);
}

void variable_value(unsigned char *buf,
                                   unsigned char token,
                                   unsigned char message_id_msb,
                                   unsigned char message_id_lsb,
                                   int return_value)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = 0x45; // response code 2.05 CONTENT
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;
  buf[5] = 0xff; // payload marker
  buf[6] = return_value >> 24;
  buf[7] = return_value >> 16 & 0xff;
  buf[8] = return_value >> 8 & 0xff;
  buf[9] = return_value & 0xff;

  memset(buf + 10, 6, 6); // PKCS #7 padding

  encrypt(buf, 16);
}

void variable_value(unsigned char *buf,
                                   unsigned char token,
                                   unsigned char message_id_msb,
                                   unsigned char message_id_lsb,
                                   double return_value)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = 0x45; // response code 2.05 CONTENT
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;
  buf[5] = 0xff; // payload marker

  memcpy(buf + 6, &return_value, 8);

  memset(buf + 14, 2, 2); // PKCS #7 padding

  encrypt(buf, 16);
}

// Returns the length of the buffer to send
int variable_value(unsigned char *buf,
                                  unsigned char token,
                                  unsigned char message_id_msb,
                                  unsigned char message_id_lsb,
                                  const void *return_value,
                                  int length)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = 0x45; // response code 2.05 CONTENT
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;
  buf[5] = 0xff; // payload marker

  memcpy(buf + 6, return_value, length);

  int msglen = 6 + length;
  int buflen = (msglen & ~15) + 16;
  char pad = buflen - msglen;
  memset(buf + msglen, pad, pad); // PKCS #7 padding

  encrypt(buf, buflen);

  return buflen;
}

uint32_t timestamp_offset;
uint32_t last_time_offset;

void set_time(uint32_t time){
  timestamp_offset = time - (millis()/1000);
  last_time_offset = millis()/1000;
}

uint32_t get_time(){
  //as long as we get time once every 98 days this should be OK
  if(millis()/1000<last_time_offset){
    timestamp_offset += 4294968;
  }
  last_time_offset = millis()/1000;
  return timestamp_offset+last_time_offset;
}

void handle_time_response(uint32_t time)
{
    // deduct latency
    uint32_t latency = last_chunk_millis ? (millis()-last_chunk_millis)/2000 : 0;
    last_chunk_millis = 0;
    set_time(time-latency);
}

int numUserFunctions(void)
{
    return funcs.size();
}

const char* getUserFunctionKey(int function_index)
{
    return funcs[function_index].userFuncKey;
}

int numUserVariables(void)
{
    return vars.size();
}

const char* getUserVariableKey(int variable_index)
{
    return vars[variable_index].userVarKey;
}

int userVarType(const char *varKey)
{
    User_Var_Lookup_Table_t* item = find_var_by_key(varKey);
    return item ? item->userVarType : -1;
}

SparkReturnType::Enum wrapVarTypeInEnum(const char *varKey)
{
    switch (userVarType(varKey))
    {
        case 1:
            return SparkReturnType::BOOLEAN;
        case 4:
            return SparkReturnType::STRING;
        case 9:
            return SparkReturnType::DOUBLE;
        case 2:
        default:
            return SparkReturnType::INT;
    }
}

bool send_subscription(const char *event_name, const char *device_id)
{
  uint16_t msg_id = next_message_id();
  size_t msglen = subscription(queue + 2, msg_id, event_name, device_id);

  size_t buflen = (msglen & ~15) + 16;
  char pad = buflen - msglen;
  memset(queue + 2 + msglen, pad, pad); // PKCS #7 padding

  encrypt(queue + 2, buflen);

  queue[0] = (buflen >> 8) & 0xff;
  queue[1] = buflen & 0xff;

  return (0 <= blocking_send(queue, buflen + 2));
}

bool send_subscription(const char *event_name,
                                      SubscriptionScope::Enum scope)
{
  uint16_t msg_id = next_message_id();
  size_t msglen = subscription(queue + 2, msg_id, event_name, scope);

  size_t buflen = (msglen & ~15) + 16;
  char pad = buflen - msglen;
  memset(queue + 2 + msglen, pad, pad); // PKCS #7 padding

  encrypt(queue + 2, buflen);

  queue[0] = (buflen >> 8) & 0xff;
  queue[1] = buflen & 0xff;

  return (0 <= blocking_send(queue, buflen + 2));
}

void send_subscriptions()
{
  const int NUM_HANDLERS = sizeof(event_handlers) / sizeof(FilteringEventHandler);
  for (int i = 0; i < NUM_HANDLERS; i++)
  {
    if (NULL != event_handlers[i].handler)
    {
        if (event_handlers[i].device_id[0])
        {
            send_subscription(event_handlers[i].filter, event_handlers[i].device_id);
        }
        else
        {
            send_subscription(event_handlers[i].filter, event_handlers[i].scope);
        }
    }
  }
}

bool event_handler_exists(const char *event_name, EventHandler handler,
    void *handler_data, SubscriptionScope::Enum scope, const char* id)
{
  const int NUM_HANDLERS = sizeof(event_handlers) / sizeof(FilteringEventHandler);
  for (int i = 0; i < NUM_HANDLERS; i++)
  {
      if (event_handlers[i].handler==handler &&
          event_handlers[i].handler_data==handler_data &&
          event_handlers[i].scope==scope) {
        const size_t MAX_FILTER_LEN = sizeof(event_handlers[i].filter);
        const size_t FILTER_LEN = strnlen(event_name, MAX_FILTER_LEN);
        if (!strncmp(event_handlers[i].filter, event_name, FILTER_LEN)) {
            const size_t MAX_ID_LEN = sizeof(event_handlers[i].device_id)-1;
            const size_t id_len = id ? strnlen(id, MAX_ID_LEN) : 0;
            if (id_len)
                return !strncmp(event_handlers[i].device_id, id, id_len);
            else
                return !event_handlers[i].device_id[0];
        }
      }
  }
  return false;
}

bool add_event_handler(const char *event_name, EventHandler handler,
    void *handler_data, SubscriptionScope::Enum scope, const char* id)
{
    if (event_handler_exists(event_name, handler, handler_data, scope, id))
        return true;

  const int NUM_HANDLERS = sizeof(event_handlers) / sizeof(FilteringEventHandler);
  for (int i = 0; i < NUM_HANDLERS; i++)
  {
    if (NULL == event_handlers[i].handler)
    {
      const size_t MAX_FILTER_LEN = sizeof(event_handlers[i].filter);
      const size_t FILTER_LEN = strnlen(event_name, MAX_FILTER_LEN);
      memcpy(event_handlers[i].filter, event_name, FILTER_LEN);
      memset(event_handlers[i].filter + FILTER_LEN, 0, MAX_FILTER_LEN - FILTER_LEN);
      event_handlers[i].handler = handler;
      event_handlers[i].handler_data = handler_data;
      event_handlers[i].device_id[0] = 0;
        const size_t MAX_ID_LEN = sizeof(event_handlers[i].device_id)-1;
        const size_t id_len = id ? strnlen(id, MAX_ID_LEN) : 0;
        memcpy(event_handlers[i].device_id, id, id_len);
        event_handlers[i].device_id[id_len] = 0;
        event_handlers[i].scope = scope;
      return true;
    }
  }
  return false;
}


const void *getUserVar(const char *varKey)
{
    User_Var_Lookup_Table_t* item = find_var_by_key(varKey);
    const void* result = nullptr;
    if (item) {
      if (item->update)
            result = item->update(item->userVarKey, item->userVarType, item->userVar, nullptr);
      else
            result = item->userVar;
    }
    return result;
}

void userFuncScheduleImpl(User_Func_Lookup_Table_t* item, const char* paramString, bool freeParamString, FunctionResultCallback callback)
{
    int result = item->pUserFunc(item->pUserFuncData, paramString, NULL);
    if (freeParamString)
        delete paramString;

    callback((const void*)long(result), SparkReturnType::INT);
}

int userFuncSchedule(const char *funcKey, const char *paramString, FunctionResultCallback callback, void* reserved)
{
    // for now, we invoke the function directly and return the result via the callback
    User_Func_Lookup_Table_t* item = find_func_by_key(funcKey);
    if (!item)
        return -1;

    userFuncScheduleImpl(item, paramString, false, callback);

    return 0;
}


SubscriptionScope::Enum convert(Spark_Subscription_Scope_TypeDef subscription_type)
{
    return(subscription_type==MY_DEVICES) ? SubscriptionScope::MY_DEVICES : SubscriptionScope::FIREHOSE;
}

bool register_event(const char* eventName, SubscriptionScope::Enum event_scope, const char* deviceID)
{
    bool success;
    if (deviceID)
        success = send_subscription(eventName, deviceID);
    else
        success = send_subscription(eventName, event_scope);
    return success;
}

bool spark_subscribe(const char *eventName, EventHandler handler, void* handler_data,
        Spark_Subscription_Scope_TypeDef scope, const char* deviceID, void* reserved)
{
    //SYSTEM_THREAD_CONTEXT_SYNC(spark_subscribe(eventName, handler, handler_data, scope, deviceID, reserved));
    auto event_scope = convert(scope);
    bool success = add_event_handler(eventName, handler, handler_data, event_scope, deviceID);
    if (success && spark_connected())
    {
        register_event(eventName, event_scope, deviceID);
    }
    return success;
}


inline EventType::Enum convert(Spark_Event_TypeDef eventType) {
    return eventType==PUBLIC ? EventType::PUBLIC : EventType::PRIVATE;
}

inline bool is_system(const char* event_name) {
    // if there were a strncmpi this would be easier!
    char prefix[6];
    if (!*event_name || strlen(event_name)<5)
        return false;
    memcpy(prefix, event_name, 5);
    prefix[5] = '\0';
    return !strcasecmp(prefix, "spark");
}

// Returns true on success, false on sending timeout or rate-limiting failure
bool send_event(const char *event_name, const char *data,
                               int ttl, EventType::Enum event_type)
{
  if (updating)
  {
    return false;
  }

  bool is_system_event = is_system(event_name);

  if (is_system_event) {
      static uint16_t lastMinute = 0;
      static uint8_t eventsThisMinute = 0;

      uint16_t currentMinute = uint16_t(millis()>>16);
      if (currentMinute==lastMinute) {      // == handles millis() overflow
          if (eventsThisMinute==255)
              return false;
      }
      else {
          lastMinute = currentMinute;
          eventsThisMinute = 0;
      }
      eventsThisMinute++;
  }
  else {
    static uint32_t recent_event_ticks[5] = {
      (uint32_t) -1000, (uint32_t) -1000,
      (uint32_t) -1000, (uint32_t) -1000,
      (uint32_t) -1000 };
    static int evt_tick_idx = 0;

    uint32_t now = recent_event_ticks[evt_tick_idx] = millis();
    evt_tick_idx++;
    evt_tick_idx %= 5;
    if (now - recent_event_ticks[evt_tick_idx] < 1000)
    {
      // exceeded allowable burst of 4 events per second
      return false;
    }
  }
  uint16_t msg_id = next_message_id();
  size_t msglen = Messages::event(queue + 2, msg_id, event_name, data, ttl, event_type, false);
  size_t wrapped_len = wrap(queue, msglen);

  return (0 <= blocking_send(queue, wrapped_len));
}

bool spark_send_event(const char* name, const char* data, int ttl, Spark_Event_TypeDef eventType, void* reserved)
{
    //SYSTEM_THREAD_CONTEXT_SYNC(spark_send_event(name, data, ttl, eventType, reserved));

    //return spark_protocol_send_event(sp, name, data, ttl, convert(eventType), NULL);
    return send_event(name, data, ttl, convert(eventType));
}


bool spark_variable(const char *varKey, const void *userVar, Spark_Data_TypeDef userVarType, spark_variable_t* extra)
{
    //SYSTEM_THREAD_CONTEXT_SYNC(spark_variable(varKey, userVar, userVarType, extra));

    User_Var_Lookup_Table_t* item = NULL;
    if (NULL != userVar && NULL != varKey && strlen(varKey)<=USER_VAR_KEY_LENGTH)
    {
        if ((item=find_var_by_key_or_add(varKey))!=NULL)
        {
            item->userVar = userVar;
            item->userVarType = userVarType;
            if (extra) {
                item->update = extra->update;
            }
            memset(item->userVarKey, 0, USER_VAR_KEY_LENGTH);
            memcpy(item->userVarKey, varKey, USER_VAR_KEY_LENGTH);
        }
    }
    return item!=NULL;
}

void function_return(unsigned char *buf,
                                    unsigned char token,
                                    int return_value)
{
  unsigned short message_id = next_message_id();

  buf[0] = 0x51; // non-confirmable, one-byte token
  buf[1] = 0x44; // response code 2.04 CHANGED
  buf[2] = message_id >> 8;
  buf[3] = message_id & 0xff;
  buf[4] = token;
  buf[5] = 0xff; // payload marker
  buf[6] = return_value >> 24;
  buf[7] = return_value >> 16 & 0xff;
  buf[8] = return_value >> 8 & 0xff;
  buf[9] = return_value & 0xff;

  memset(buf + 10, 6, 6); // PKCS #7 padding

  encrypt(buf, 16);
}



bool spark_function_internal(const cloud_function_descriptor* desc, void* reserved)
{
    User_Func_Lookup_Table_t* item = NULL;
    if (NULL != desc->fn && NULL != desc->funcKey && strlen(desc->funcKey)<=USER_FUNC_KEY_LENGTH)
    {
        if ((item=find_func_by_key(desc->funcKey)) || (item = funcs.add()))
        {
            item->pUserFunc = desc->fn;
            item->pUserFuncData = desc->data;
            memset(item->userFuncKey, 0, USER_FUNC_KEY_LENGTH);
            memcpy(item->userFuncKey, desc->funcKey, USER_FUNC_KEY_LENGTH);
        }
    }
    return item!=NULL;
}

/**
 * This is the original released signature for firmware version 0 and needs to remain like this.
 * (The original returned void - we can safely change to bool.)
 */
bool spark_function(const char *funcKey, p_user_function_int_str_t pFunc, void* reserved)
{
    //SYSTEM_THREAD_CONTEXT_SYNC(spark_function(funcKey, pFunc, reserved));

    bool result;
    if (funcKey) {                          // old call, with funcKey != NULL
        cloud_function_descriptor desc;
        desc.funcKey = funcKey;
        desc.fn = call_raw_user_function;
        desc.data = (void*)pFunc;
        result = spark_function_internal(&desc, NULL);
    }
    else {      // new call - pFunc is actually a pointer to a descriptor
        result = spark_function_internal((cloud_function_descriptor*)pFunc, reserved);
    }
    return result;
}

bool register_function(cloud_function_t fn, void* data, const char* funcKey)
{
    cloud_function_descriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.fn = fn;
    desc.data = (void*)data;
    desc.funcKey = funcKey;
    return spark_function(NULL, (user_function_int_str_t*)&desc, NULL);
}

String buffer_to_string(const uint8_t *buf,size_t length){
  String result = "";
  for(uint8_t i = 0; i<length; i++){
    result += buf[i];
  }
  return result;
}

int description(unsigned char *buf, unsigned char token,
                               unsigned char message_id_msb, unsigned char message_id_lsb, int desc_flags)
{
    buf[0] = 0x61; // acknowledgment, one-byte token
    buf[1] = 0x45; // response code 2.05 CONTENT
    buf[2] = message_id_msb;
    buf[3] = message_id_lsb;
    buf[4] = token;
    buf[5] = 0xff; // payload marker

    BufferAppender appender(buf+6, QUEUE_SIZE-8);
    appender.append("{");
    bool has_content = false;

    if (desc_flags && DESCRIBE_APPLICATION) {
        has_content = true;
      appender.append("\"f\":[");

      int num_keys = numUserFunctions();
      int i;
      for (i = 0; i < num_keys; ++i)
      {
        if (i)
        {
            appender.append(',');
        }
        appender.append('"');

        const char* key = getUserFunctionKey(i);
        size_t function_name_length = strlen(key);
        if (MAX_FUNCTION_KEY_LENGTH < function_name_length)
        {
          function_name_length = MAX_FUNCTION_KEY_LENGTH;
        }
        appender.append((const uint8_t*)key, function_name_length);
        appender.append('"');
      }

      appender.append("],\"v\":{");

      num_keys = numUserVariables();
      for (i = 0; i < num_keys; ++i)
      {
        if (i)
        {
            appender.append(',');
        }
        appender.append('"');
        const char* key = getUserVariableKey(i);
        size_t variable_name_length = strlen(key);
        SparkReturnType::Enum t = wrapVarTypeInEnum(key);
        if (MAX_VARIABLE_KEY_LENGTH < variable_name_length)
        {
          variable_name_length = MAX_VARIABLE_KEY_LENGTH;
        }
        appender.append((const uint8_t*)key, variable_name_length);
        appender.append("\":");
        appender.append('0' + (char)t);
      }
      appender.append('}');
    }

    if ((desc_flags&DESCRIBE_SYSTEM)) {
    //if (descriptor.append_system_info && (desc_flags&DESCRIBE_SYSTEM)) {
      if (has_content)
        appender.append(',');
      //descriptor.append_system_info(append_instance, &appender, NULL);
      appender.append("\"p\":82,\"m\":[]");
    }
    appender.append('}');

    int msglen = appender.next() - (uint8_t *)buf;


    int buflen = (msglen & ~15) + 16;
    char pad = buflen - msglen;
    memset(buf+msglen, pad, pad); // PKCS #7 padding

    encrypt(buf, buflen);
    return buflen;
}
/*
int description(unsigned char *buf, unsigned char token,
                               unsigned char message_id_msb, unsigned char message_id_lsb, int desc_flags)
{
    buf[0] = 0x61; // acknowledgment, one-byte token
    buf[1] = 0x45; // response code 2.05 CONTENT
    buf[2] = message_id_msb;
    buf[3] = message_id_lsb;
    buf[4] = token;
    buf[5] = 0xff; // payload marker

    String content = "";

    //BufferAppender appender(buf+6, QUEUE_SIZE-8);
    content += "{";
    bool has_content = false;

    if (desc_flags && DESCRIBE_APPLICATION) {
        has_content = true;
      content += "\"f\":[";

      int num_keys = numUserFunctions();
      int i;
      for (i = 0; i < num_keys; ++i)
      {
        if (i)
        {
            content += ",";
        }
        content += "\"";

        const char* key = getUserFunctionKey(i);
        size_t function_name_length = strlen(key);
        if (MAX_FUNCTION_KEY_LENGTH < function_name_length)
        {
          function_name_length = MAX_FUNCTION_KEY_LENGTH;
        }
        content += buffer_to_string((const uint8_t*)key, function_name_length);
        content += "\"";
      }

      content += "],\"v\":{";

      num_keys = numUserVariables();
      for (i = 0; i < num_keys; ++i)
      {
        if (i)
        {
            content += ",";
        }
        content += "\"";
        const char* key = getUserVariableKey(i);
        size_t variable_name_length = strlen(key);
        SparkReturnType::Enum t = wrapVarTypeInEnum(key);
        if (MAX_VARIABLE_KEY_LENGTH < variable_name_length)
        {
          variable_name_length = MAX_VARIABLE_KEY_LENGTH;
        }
        content += buffer_to_string((const uint8_t*)key, variable_name_length);
        content += "\":";
        content += String('0' + (char)t);
      }
      content += "}";
    }

    if (desc_flags&DESCRIBE_SYSTEM) {
      if (has_content)
        content += ",";
      //descriptor.append_system_info(append_instance, &appender, NULL);
      content += "\"p\":82,\"m\":[]";
    }
    content += "}";

    
    //truncate if too long
     if(content.length() > QUEUE_SIZE-8){
      content = content.substring(0,QUEUE_SIZE-8);
    }

    int msglen = ((uint8_t *)buf+6 + content.length()) - (uint8_t *)buf;


    int buflen = (msglen & ~15) + 16;
    char pad = buflen - msglen;
    memset(buf+msglen, pad, pad); // PKCS #7 padding

    encrypt(buf, buflen);
    return buflen;
}

*/
bool function_result(const void* result, SparkReturnType::Enum, uint8_t token)
{
    // send return value
    queue[0] = 0;
    queue[1] = 16;
    function_return(queue + 2, token, long(result));
    if (0 > blocking_send(queue, 18))
    {
      // error
      return false;
    }
    return true;
}

char function_arg[MAX_FUNCTION_ARG_LENGTH];

bool handle_function_call(msg& message)
{
    // copy the function key
    char function_key[13];
    memset(function_key, 0, 13);
    int function_key_length = queue[7] & 0x0F;
    memcpy(function_key, queue + 8, function_key_length);

    // How long is the argument?
    size_t q_index = 8 + function_key_length;
    size_t query_length = queue[q_index] & 0x0F;
    if (13 == query_length)
    {
      ++q_index;
      query_length = 13 + queue[q_index];
    }
    else if (14 == query_length)
    {
      ++q_index;
      query_length = queue[q_index] << 8;
      ++q_index;
      query_length |= queue[q_index];
      query_length += 269;
    }

    bool has_function = false;

    // allocated memory bounds check
    if (MAX_FUNCTION_ARG_LENGTH > query_length)
    {
        // save a copy of the argument
        memcpy(function_arg, queue + q_index + 1, query_length);
        function_arg[query_length] = 0; // null terminate string
        has_function = true;
    }

    uint8_t* msg_to_send = message.response;
    // send ACK
    msg_to_send[0] = 0;
    msg_to_send[1] = 16;
    coded_ack(msg_to_send + 2, has_function ? 0x00 : RESPONSE_CODE(4,00), queue[2], queue[3]);
    if (0 > blocking_send(msg_to_send, 18))
    {
      // error
      return false;
    }

    // call the given user function
    auto callback = [=] (const void* result, SparkReturnType::Enum resultType ) { return function_result(result, resultType, message.token); };
    userFuncSchedule(function_key, function_arg, callback, NULL);
    return true;
}


void invokeEventHandlerInternal(uint16_t handlerInfoSize, FilteringEventHandler* handlerInfo,
                const char* event_name, const char* data, void* reserved)
{
    if(handlerInfo->handler_data)
    {
        EventHandlerWithData handler = (EventHandlerWithData) handlerInfo->handler;
        handler(handlerInfo->handler_data, event_name, data);
    }
    else
    {
        handlerInfo->handler(event_name, data);
    }
}

void invokeEventHandlerString(uint16_t handlerInfoSize, FilteringEventHandler* handlerInfo,
                const String& name, const String& data, void* reserved)
{
    invokeEventHandlerInternal(handlerInfoSize, handlerInfo, name.c_str(), data.c_str(), reserved);
}


void invokeEventHandler(uint16_t handlerInfoSize, FilteringEventHandler* handlerInfo,
                const char* event_name, const char* event_data, void* reserved)
{
    invokeEventHandlerInternal(handlerInfoSize, handlerInfo, event_name, event_data, reserved);
    
}

volatile uint32_t lastCloudEvent = 0;

void handle_event(msg& message)
{
    const unsigned len = message.len;

    // fist decode the event data before looking for a handler
    unsigned char pad = queue[len - 1];
    if (0 == pad || 16 < pad)
    {
        // ignore bad message, PKCS #7 padding must be 1-16
        return;
    }
    // end of CoAP message
    unsigned char *end = queue + len - pad;

    unsigned char *event_name = queue + 6;
    size_t event_name_length = CoAP::option_decode(&event_name);
    if (0 == event_name_length)
    {
        // error, malformed CoAP option
        return;
    }

    unsigned char *next_src = event_name + event_name_length;
    unsigned char *next_dst = next_src;
    while (next_src < end && 0x00 == (*next_src & 0xf0))
    {
      // there's another Uri-Path option, i.e., event name with slashes
      size_t option_len = CoAP::option_decode(&next_src);
      *next_dst++ = '/';
      if (next_dst != next_src)
      {
        // at least one extra byte has been used to encode a CoAP Uri-Path option length
        memmove(next_dst, next_src, option_len);
      }
      next_src += option_len;
      next_dst += option_len;
    }
    event_name_length = next_dst - event_name;

    if (next_src < end && 0x30 == (*next_src & 0xf0))
    {
      // Max-Age option is next, which we ignore
      size_t next_len = CoAP::option_decode(&next_src);
      next_src += next_len;
    }

    unsigned char *data = NULL;
    if (next_src < end && 0xff == *next_src)
    {
      // payload is next
      data = next_src + 1;
      // null terminate data string
      *end = 0;
    }
    // null terminate event name string
    event_name[event_name_length] = 0;

  const int NUM_HANDLERS = sizeof(event_handlers) / sizeof(FilteringEventHandler);
  for (int i = 0; i < NUM_HANDLERS; i++)
  {
    if (NULL == event_handlers[i].handler)
    {
       break;
    }
    const size_t MAX_FILTER_LENGTH = sizeof(event_handlers[i].filter);
    const size_t filter_length = strnlen(event_handlers[i].filter, MAX_FILTER_LENGTH);

    if (event_name_length < filter_length)
    {
      // does not match this filter, try the next event handler
      continue;
    }

    const int cmp = memcmp(event_handlers[i].filter, event_name, filter_length);
    if (0 == cmp)
    {
        // don't call the handler directly, use a callback for it.
        if (!invokeEventHandler)
        {
            if(event_handlers[i].handler_data)
            {
                EventHandlerWithData handler = (EventHandlerWithData) event_handlers[i].handler;
                handler(event_handlers[i].handler_data, (char *)event_name, (char *)data);
            }
            else
            {
                event_handlers[i].handler((char *)event_name, (char *)data);
            }
        }
        else
        {
            invokeEventHandler(sizeof(FilteringEventHandler), &event_handlers[i], (const char*)event_name, (const char*)data, NULL);
        }
    }
    // else continue the for loop to try the next handler
  }
}

bool send_description(int description_flags, msg& message)
{
    int desc_len = description(queue + 2, message.token, queue[2], queue[3], description_flags);
    queue[0] = (desc_len >> 8) & 0xff;
    queue[1] = desc_len & 0xff;
    return blocking_send(queue, desc_len + 2)>=0;
}


void empty_ack(unsigned char *buf,
                          unsigned char message_id_msb,
                          unsigned char message_id_lsb) {
        coded_ack(buf, 0, message_id_msb, message_id_lsb);
    };






FileTransfer::Descriptor file;
unsigned chunk_bitmap_size()
{
    return (file.chunk_count(chunk_size)+7)/8;
}

uint8_t* chunk_bitmap()
{
    return &queue[QUEUE_SIZE-chunk_bitmap_size()];
}

chunk_index_t missed_chunk_index;

void separate_response_with_payload(unsigned char *buf,
                                      unsigned char token,
                                      unsigned char code,
                                      unsigned char* payload,
                                      unsigned payload_len)
{
  unsigned short message_id = next_message_id();

  buf[0] = 0x51; // non-confirmable, one-byte token
  buf[1] = code;
  buf[2] = message_id >> 8;
  buf[3] = message_id & 0xff;
  buf[4] = token;

  unsigned len = 5;
  // for now, assume the payload is less than 9
  if (payload && payload_len) {
      buf[5] = 0xFF;
      memcpy(buf+6, payload, payload_len);
      len += 1 + payload_len;
  }

  memset(buf + len, 16-len, 16-len); // PKCS #7 padding

  encrypt(buf, 16);
}


inline bool is_chunk_received(chunk_index_t idx)
{
    return (chunk_bitmap()[idx>>3] & uint8_t(1<<(idx&7)));
}


void separate_response(unsigned char *buf,
                                      unsigned char token,
                                      unsigned char code)
{
    separate_response_with_payload(buf, token, code, NULL, 0);
}

chunk_index_t next_chunk_missing(chunk_index_t start)
{
    chunk_index_t chunk = NO_CHUNKS_MISSING;
    chunk_index_t chunks = file.chunk_count(chunk_size);
    chunk_index_t idx = start;
    for (;idx<chunks; idx++)
    {
        if (!is_chunk_received(idx))
        {
            //serial_dump("next missing chunk %d from %d", idx, start);
            chunk = idx;
            break;
        }
    }
    return chunk;
}

void chunk_received(unsigned char *buf,
                                   unsigned char token,
                                   ChunkReceivedCode::Enum code)
{
  separate_response(buf, token, code);
}


int send_missing_chunks(int count)
{
    int sent = 0;
    chunk_index_t idx = 0;

    uint8_t* buf = queue+2;
    unsigned short message_id = next_message_id();
    buf[0] = 0x40; // confirmable, no token
    buf[1] = 0x01; // code 0.01 GET
    buf[2] = message_id >> 8;
    buf[3] = message_id & 0xff;
    buf[4] = 0xb1; // one-byte Uri-Path option
    buf[5] = 'c';
    buf[6] = 0xff; // payload marker

    while ((idx=next_chunk_missing(chunk_index_t(idx)))!=NO_CHUNKS_MISSING && sent<count)
    {
        buf[(sent*2)+7] = idx >> 8;
        buf[(sent*2)+8] = idx & 0xFF;

        missed_chunk_index = idx;
        idx++;
        sent++;
    }

    if (sent>0) {
        //serial_dump("Sent %d missing chunks", sent);

        size_t message_size = 7+(sent*2);
        message_size = wrap(queue, message_size);
        if (0 > blocking_send(queue, message_size))
            return -1;
    }
    return sent;
}

void chunk_missed(unsigned char *buf, unsigned short chunk_index)
{
  unsigned short message_id = next_message_id();

  buf[0] = 0x40; // confirmable, no token
  buf[1] = 0x01; // code 0.01 GET
  buf[2] = message_id >> 8;
  buf[3] = message_id & 0xff;
  buf[4] = 0xb1; // one-byte Uri-Path option
  buf[5] = 'c';
  buf[6] = 0xff; // payload marker
  buf[7] = chunk_index >> 8;
  buf[8] = chunk_index & 0xff;

  memset(buf + 9, 7, 7); // PKCS #7 padding

  encrypt(buf, 16);
}

void update_ready(unsigned char *buf, unsigned char token)
{
    separate_response_with_payload(buf, token, 0x44, NULL, 0);
}

void update_ready(unsigned char *buf, unsigned char token, uint8_t flags)
{
    separate_response_with_payload(buf, token, 0x44, &flags, 1);
}



static uint8 calc_boot_chksum(uint8 *start, uint8 *end) {
    uint8 chksum = CHKSUM_INIT;
    while(start < end) {
        chksum ^= *start;
        start++;
    }
    return chksum;
}


bool readBootConfig(){
    noInterrupts();
    spi_flash_read(BOOT_CONFIG_SECTOR * SECTOR_SIZE, reinterpret_cast<uint32_t*>(boot_buffer), BOOT_CONFIG_SIZE);
    
    if(bootConfig->magic != BOOT_CONFIG_MAGIC || bootConfig->chksum != calc_boot_chksum((uint8*)bootConfig, (uint8*)&bootConfig->chksum)){
        
        //load the backup and copy to main
        spi_flash_read(BOOT_BACKUP_CONFIG_SECTOR * SECTOR_SIZE, reinterpret_cast<uint32_t*>(boot_buffer), BOOT_CONFIG_SIZE);
        spi_flash_erase_sector(BOOT_CONFIG_SECTOR);
        spi_flash_write(BOOT_CONFIG_SECTOR * SECTOR_SIZE, reinterpret_cast<uint32_t*>(boot_buffer), BOOT_CONFIG_SIZE);
        
    }
    interrupts();
    
    if(bootConfig->magic != BOOT_CONFIG_MAGIC || bootConfig->chksum != calc_boot_chksum((uint8*)bootConfig, (uint8*)&bootConfig->chksum)){
        return false;
    }
    
    return true;
}

void writeBootConfig(){
    noInterrupts();
    bootConfig->chksum = calc_boot_chksum((uint8*)bootConfig,(uint8*)&bootConfig->chksum);
    spi_flash_erase_sector(BOOT_CONFIG_SECTOR);
    spi_flash_write(BOOT_CONFIG_SECTOR * SECTOR_SIZE, reinterpret_cast<uint32_t*>(boot_buffer), BOOT_CONFIG_SIZE);
    spi_flash_erase_sector(BOOT_BACKUP_CONFIG_SECTOR);
    spi_flash_write(BOOT_BACKUP_CONFIG_SECTOR * SECTOR_SIZE, reinterpret_cast<uint32_t*>(boot_buffer), BOOT_CONFIG_SIZE);
    interrupts();
    
}




#define FLASH_MAX_SIZE          (unsigned long)(0x100000 - 0x2000)
#define OTA_CHUNK_SIZE          (unsigned long)512

uint32_t getOTAFlashSlot(){
    if(bootConfig->program_rom != 0 && bootConfig->config_rom != 0)
      return 0;
    else if(bootConfig->program_rom != 4 && bootConfig->config_rom != 4)
      return 4;
    else
      return 8;
}

bool status_led_state = false;
#define STATUS_LED 5
void LED_Toggle(){
  status_led_state = !status_led_state;
  digitalWrite(STATUS_LED, status_led_state);
}

int prepare_for_firmware_update(FileTransfer::Descriptor& file, uint32_t flags, void* reserved)
{
    Serial.println("FIRMWARE");
    file.file_address = bootConfig->roms[getOTAFlashSlot()];// + file.chunk_address;

    // chunk_size 0 indicates defaults.
    if (file.chunk_size==0) {
        file.chunk_size = OTA_CHUNK_SIZE;
        file.file_length = FLASH_MAX_SIZE;
    }

    int result = 0;
    if (flags & 1) {
        // only check address
    }
    else {
        pinMode(STATUS_LED, OUTPUT);
        if (file.store!=FileTransfer::Store::FIRMWARE)
          return 1;
    }
    return result;
}

void set_chunks_received(uint8_t value)
{
    size_t bytes = chunk_bitmap_size();
    if (bytes)
      memset(queue+QUEUE_SIZE-bytes, value, bytes);
}

uint16_t last_chunk = 0;

void spark_process();

bool handle_update_begin(msg& message)
{
    // send ACK
    uint8_t* msg_to_send = message.response;
    *msg_to_send = 0;
    *(msg_to_send + 1) = 16;

    uint8_t flags = 0;
    int actual_len = message.len - queue[message.len-1];
    if (actual_len>=20 && queue[7]==0xFF) {
        flags = decode_uint8(queue+8);
        file.chunk_size = decode_uint16(queue+9);
        file.file_length = decode_uint32(queue+11);
        file.store = FileTransfer::Store::Enum(decode_uint8(queue+15));
        file.file_address = decode_uint32(queue+16);
        file.chunk_address = file.file_address;
    }
    else {
        file.chunk_size = 0;
        file.file_length = 0;
        file.store = FileTransfer::Store::FIRMWARE;
        file.file_address = 0;
        file.chunk_address = 0;
    }

    

    // check the parameters only
    bool success = !prepare_for_firmware_update(file, 1, NULL);
    if (success) {
        success = file.chunk_count(file.chunk_size) < MAX_CHUNKS;
    }

    last_chunk = file.chunk_count(OTA_CHUNK_SIZE)-1;

    coded_ack(msg_to_send+2, success ? 0x00 : RESPONSE_CODE(4,00), queue[2], queue[3]);
    if (0 > blocking_send(msg_to_send, 18))
    {
      // error
      return false;
    }
    if (success)
    {
        if (!prepare_for_firmware_update(file, 0, NULL))
        {
            last_chunk_millis = millis();
            chunk_index = 0;
            chunk_size = file.chunk_size;   // save chunk size since the descriptor size is overwritten
            updating = 1;
            // when not in fast OTA mode, the chunk missing buffer is set to 1 since the protocol
            // handles missing chunks one by one. Also we don't know the actual size of the file to
            // know the correct size of the bitmap.
            set_chunks_received(flags & 1 ? 0 : 0xFF);
            // send update_reaady - use fast OTA if available
            update_ready(msg_to_send + 2, message.token, (flags & 0x1));
            if (0 > blocking_send(msg_to_send, 18))
            {
              // error
              return false;
            }
        }
    }

    //PUMP ONLY CLOUD WHIE UPDATING
    uint32_t update_timeout = millis()+60000;
    while(updating>0 && millis()<update_timeout){
      spark_process();
    }
    return true;
}

void spark_disconnect(){
  if(pClient.connected())
    pClient.stop();
}

int finish_firmware_update(FileTransfer::Descriptor& file, uint32_t flags, void* reserved)
{

    Serial.println("UPDATE FINISHED - REBOOT ME");
    if (flags & 1) {    // update successful

        if((file.chunk_address/SECTOR_SIZE)<((file.file_address+FLASH_MAX_SIZE)/SECTOR_SIZE)-1){ 
        //check if sector of final chunk is less then the max sector
          noInterrupts();
          spi_flash_erase_sector((file.chunk_address/SECTOR_SIZE)+1);
          interrupts();
        }

        //TODO check CRC and fall through if it fails

        Serial.println("DONE - RESTART");
        //TODO bootConfig->current_rom = getOTAFlashSlot();
        spark_disconnect();
        delay(100);
        ESP.restart();
        while(1);
        
    }

    spark_send_event("oak/device/stderr","OTA Update Failed", 60, PRIVATE, NULL); 
    delay(500);
    spark_disconnect();
    delay(100);
    ESP.restart();
    
    return 0;
}



void save_firmware_chunk(FileTransfer::Descriptor& file, uint8_t* chunk, void* reserved)
{
    //todo ensure chunk is 512, else pad to 512 to ensure alignment
    noInterrupts();
    if(file.chunk_address%SECTOR_SIZE == 0)
      spi_flash_erase_sector(file.chunk_address/SECTOR_SIZE);// != SPI_FLASH_RESULT_OK)

    spi_flash_write(file.chunk_address, reinterpret_cast<uint32_t*>(chunk), OTA_CHUNK_SIZE);
    interrupts();

    LED_Toggle();

    return;
}


inline void flag_chunk_received(chunk_index_t idx)
{
//    serial_dump("flagged chunk %d", idx);
    chunk_bitmap()[idx>>3] |= uint8_t(1<<(idx&7));
}

void notify_update_done(uint8_t* buf)
{
    unsigned short message_id = next_message_id();
    size_t size = Messages::update_done(buf+2, message_id, false);
    wrap(buf, size);
}

bool handle_chunk(msg& message)
{
  //Serial.println("CHUNK");
    last_chunk_millis = millis();

    uint8_t* msg_to_send = message.response;
    // send ACK
    *msg_to_send = 0;
    *(msg_to_send + 1) = 16;
    empty_ack(msg_to_send + 2, queue[2], queue[3]);
    if (0 > blocking_send(msg_to_send, 18))
    {
      // error
      return false;
    }
    //serial_dump("chunk");
    if (!updating) {
        //serial_dump("got chunk when not updating");
        return true;
    }

    bool fast_ota = false;
    uint8_t payload = 7;

    unsigned option = 0;
    uint32_t given_crc = 0;
    while (queue[payload]!=0xFF) {
        switch (option) {
            case 0:
                given_crc = decode_uint32(queue+payload+1);
                break;
            case 1:
                chunk_index = decode_uint16(queue+payload+1);
                fast_ota = true;
                break;
        }
        option++;
        payload += (queue[payload]&0xF)+1;  // increase by the size. todo handle > 11
    }
    if (0xFF==queue[payload])
    {
        payload++;
        uint8_t* chunk = queue+payload;
        file.chunk_size = message.len - payload - queue[message.len - 1];   // remove length added due to pkcs #7 padding?
        file.chunk_address  = file.file_address + (chunk_index * chunk_size);
        if (chunk_index>=MAX_CHUNKS) {
            //serial_dump("invalid chunk index %d", chunk_index);
            return false;
        }
        uint32_t crc = crc32(chunk, file.chunk_size);
        bool has_response = false;
        bool crc_valid = (crc == given_crc);
        //todo comment out
        Serial.printf("chunk idx=%d crc=%d fast=%d updating=%d", chunk_index, crc_valid, fast_ota, updating);
        if (crc_valid)
        {

            save_firmware_chunk(file, chunk, NULL);
            if (!fast_ota || (updating!=2 && (true || (chunk_index & 32)==0))) {
                chunk_received(msg_to_send + 2, message.token, ChunkReceivedCode::OK);
                has_response = true;
            }
            flag_chunk_received(chunk_index);
            if (updating==2) {                      // clearing up missed chunks at the end of fast OTA
                chunk_index_t next_missed = next_chunk_missing(0);
                if (next_missed==NO_CHUNKS_MISSING) {
                    
                    notify_update_done(msg_to_send);
                    finish_firmware_update(file, 1, NULL);
                    
                    has_response = true;
                }
                else {
                    if (has_response && 0 > blocking_send(msg_to_send, 18)) {

                        //serial_dump("send chunk response failed");
                        return false;
                    }
                    has_response = false;

                    if (next_missed>missed_chunk_index)
                        send_missing_chunks(MISSED_CHUNKS_TO_SEND);
                }
            }
            chunk_index++;
        }
        else if (!fast_ota)
        {
            chunk_received(msg_to_send + 2, message.token, ChunkReceivedCode::BAD);
            has_response = true;
            //serial_dump("chunk bad %d", chunk_index);
        }
        // fast OTA will request the chunk later

        if (has_response && 0 > blocking_send(msg_to_send, 18))
        {
          // error
          return false;
        }
    }

    return true;
}




bool handle_update_done(msg& message)
{
    // send ACK 2.04
    uint8_t* msg_to_send = message.response;

    *msg_to_send = 0;
    *(msg_to_send + 1) = 16;
    Serial.println("update done received");
    chunk_index_t index = next_chunk_missing(0);
    bool missing = index!=NO_CHUNKS_MISSING;
    coded_ack(msg_to_send + 2, message.token, missing ? ChunkReceivedCode::BAD : ChunkReceivedCode::OK, queue[2], queue[3]);
    if (0 > blocking_send(msg_to_send, 18))
    {
        // error
        return false;
    }

    if (!missing) {
        Serial.println("update done - all done!");
        finish_firmware_update(file, 1, NULL);
    }
    else {
        updating = 2;       // flag that we are sending missing chunks.
        Serial.println("update done - missing chunks");
        send_missing_chunks(MISSED_CHUNKS_TO_SEND);
        last_chunk_millis = millis();
    }
    return true;
}








bool handle_message(msg& message, token_t token, CoAPMessageType::Enum message_type)
{
  switch (message_type)
  {

    case CoAPMessageType::DESCRIBE:
    {
        if (!send_description(DESCRIBE_SYSTEM, message) || !send_description(DESCRIBE_APPLICATION, message)) {
            return false;
        }
        break;
    }
    case CoAPMessageType::FUNCTION_CALL:
        if (!handle_function_call(message))
            return false;
        break;
    case CoAPMessageType::VARIABLE_REQUEST:
    {
      // copy the variable key
      int variable_key_length = queue[7] & 0x0F;
      if (12 < variable_key_length)
        variable_key_length = 12;

      char variable_key[13];
      memcpy(variable_key, queue + 8, variable_key_length);
      memset(variable_key + variable_key_length, 0, 13 - variable_key_length);

      queue[0] = 0;
      queue[1] = 16; // default buffer length

      // get variable value according to type using the descriptor
      SparkReturnType::Enum var_type = wrapVarTypeInEnum(variable_key);
      if(SparkReturnType::BOOLEAN == var_type)
      {
        bool *bool_val = (bool *)getUserVar(variable_key);
        variable_value(queue + 2, token, queue[2], queue[3], *bool_val);
      }
      else if(SparkReturnType::INT == var_type)
      {
        int *int_val = (int *)getUserVar(variable_key);
        variable_value(queue + 2, token, queue[2], queue[3], *int_val);
      }
      else if(SparkReturnType::STRING == var_type)
      {
        char *str_val = (char *)getUserVar(variable_key);

        // 2-byte leading length, 16 potential padding bytes
        int max_length = QUEUE_SIZE - 2 - 16;
        int str_length = strlen(str_val);
        if (str_length > max_length) {
          str_length = max_length;
        }

        int buf_size = variable_value(queue + 2, token, queue[2], queue[3], str_val, str_length);
        queue[1] = buf_size & 0xff;
        queue[0] = (buf_size >> 8) & 0xff;
      }
      else if(SparkReturnType::DOUBLE == var_type)
      {
        double *double_val = (double *)getUserVar(variable_key);
        variable_value(queue + 2, token, queue[2], queue[3], *double_val);
      }

      // buffer length may have changed if variable is a long string
      if (0 > blocking_send(queue, (queue[0] << 8) + queue[1] + 2))
      {
        // error
        return false;
      }
      break;
    }

    case CoAPMessageType::SAVE_BEGIN:
      // fall through
    case CoAPMessageType::UPDATE_BEGIN:
       return handle_update_begin(message);

    case CoAPMessageType::CHUNK:
       return handle_chunk(message);

    case CoAPMessageType::UPDATE_DONE:
       return handle_update_done(message);

    case CoAPMessageType::EVENT:
        handle_event(message);
          break;
    case CoAPMessageType::KEY_CHANGE:
      // TODO
      break;

    case CoAPMessageType::SIGNAL_START:
      queue[0] = 0;
      queue[1] = 16;
      coded_ack(queue + 2, token, ChunkReceivedCode::OK, queue[2], queue[3]);
      if (0 > blocking_send(queue, 18))
      {
        // error
        return false;
      }

      //callbacks.signal(true, 0, NULL);
      break;
    case CoAPMessageType::SIGNAL_STOP:
      queue[0] = 0;
      queue[1] = 16;
      coded_ack(queue + 2, token, ChunkReceivedCode::OK, queue[2], queue[3]);
      if (0 > blocking_send(queue, 18))
      {
        // error
        return false;
      }

      //callbacks.signal(false, 0, NULL);
      break;

    case CoAPMessageType::HELLO:
      if(deviceConfig->ota_success == 1){
        deviceConfig->ota_success = 0;
        writeDeviceConfig();
      }
      break;

    case CoAPMessageType::TIME:
      handle_time_response(queue[6] << 24 | queue[7] << 16 | queue[8] << 8 | queue[9]);
      break;

    case CoAPMessageType::PING:
      queue[0] = 0;
      queue[1] = 16;
      empty_ack(queue + 2, queue[2], queue[3]);
      if (0 > blocking_send(queue, 18))
      {
        // error
        return false;
      }
      break;

    case CoAPMessageType::EMPTY_ACK:
    case CoAPMessageType::ERROR:
    default:
      ; // drop it on the floor
  }

  // all's well
  return true;
}

CoAPMessageType::Enum received_message(unsigned char *buf, size_t length)
{
  unsigned char next_iv[16];
  memcpy(next_iv, buf, 16);

  aes_setkey_dec(&aes, key, 128);
  aes_crypt_cbc(&aes, AES_DECRYPT, length, iv_receive, buf, buf);

  memcpy(iv_receive, next_iv, 16);

  return Messages::decodeType(buf, length);
}



CoAPMessageType::Enum handle_received_message(void)
{
  last_message_millis = millis();
  expecting_ping_ack = false;
  size_t len = queue[0] << 8 | queue[1];
  if (len > QUEUE_SIZE) { // TODO add sanity check on data, e.g. CRC
      return CoAPMessageType::ERROR;
  }
  if (0 > blocking_receive(queue, len))
  {
    // error
    return CoAPMessageType::ERROR;;
  }
  CoAPMessageType::Enum message_type = received_message(queue, len);

  unsigned char token = queue[4];
  unsigned char *msg_to_send = queue + len;

  msg message;
  message.len = len;
  message.token = queue[4];
  message.response = msg_to_send;
  message.response_len = QUEUE_SIZE-len;

  return handle_message(message, token, message_type)
          ? message_type : CoAPMessageType::ERROR;
}


// Returns true if no errors and still connected.
// Returns false if there was an error, and we are probably disconnected.
bool event_loop(CoAPMessageType::Enum& message_type)
{
    message_type = CoAPMessageType::NONE;
  int bytes_received = receive(queue, 2);
  if (2 <= bytes_received)
  {
    message_type = handle_received_message();
    if (message_type==CoAPMessageType::ERROR)
    {
        if (updating) {      // was updating but had an error, inform the client
            finish_firmware_update(file, 0, NULL);
            updating = false;
        }

      // bail if and only if there was an error
      return false;
    }
  }
  else
  {
    if (0 > bytes_received)
    {
      // error, disconnected
      return false;
    }

    if (updating)
    {
      uint32_t millis_since_last_chunk = millis() - last_chunk_millis;
      if (3000 < millis_since_last_chunk)
      {
          if (updating==2) {    // send missing chunks
              //serial_dump("timeout - resending missing chunks");
              if (!send_missing_chunks(MISSED_CHUNKS_TO_SEND))
                  return false;
          }
          /* Do not resend chunks since this can cause duplicates on the server.
          else
          {
            queue[0] = 0;
            queue[1] = 16;
            chunk_missed(queue + 2, chunk_index);
            if (0 > blocking_send(queue, 18))
            {
              // error
              return false;
            }
          }
          */
          last_chunk_millis = millis();
      }
    }
    else
    {
      uint32_t millis_since_last_message = millis() - last_message_millis;
      if (expecting_ping_ack)
      {
        if (10000 < millis_since_last_message)
        {
          // timed out, disconnect
          expecting_ping_ack = false;
          last_message_millis = millis();
          ERROR("FAILED4");
          return false;
        }
      }
      else
      {
        if (15000 < millis_since_last_message)
        {
          queue[0] = 0;
          queue[1] = 16;
          ping(queue + 2);
          blocking_send(queue, 18);

          expecting_ping_ack = true;
          last_message_millis = millis();
        }
      }
    }
  }

  // no errors, still connected
  //ERROR("DID NOT FAIL");
  return true;
}

bool event_loop(CoAPMessageType::Enum message_type, uint32_t timeout)
{
    uint32_t start = millis();
    do
    {
        CoAPMessageType::Enum msgtype;
        if (!event_loop(msgtype))
            return false;
        if (msgtype==message_type)
            return true;
        // todo - ideally need a delay here
    }
    while ((millis()-start) < timeout);
    return false;
}


bool event_loop()
  {
    CoAPMessageType::Enum message;
    return event_loop(message);
  }


int handshake(){
  pClient.setNoDelay(false);
  Serial.println(pClient.status());
	memcpy(queue + 40, device_id, 12);
	int err = blocking_receive(queue, 40);;
  Serial.println(err);
	if (0 > err) { ERROR("Handshake: could not receive nonce");  return err; }

	memcpy(queue+52, deviceConfig->device_public_key,PUBLIC_KEY_LENGTH);

	rsa_context rsa;
	init_rsa_context_with_public_key(&rsa, deviceConfig->server_public_key);
	const int len = 52+PUBLIC_KEY_LENGTH;
	err = rsa_pkcs1_encrypt(&rsa, RSA_PUBLIC, len, queue, queue + len);
	rsa_free(&rsa);

	if (err) { ERROR("Handshake: rsa encrypt error"); return err; }

  Serial.println(pClient.status());

	err = blocking_send(queue + len, 256);
  if (0 > err) { Serial.println(pClient.status()); ERROR("Handshake: Unable to send key"); return err; }
	err = blocking_receive(queue, 384);
	if (0 > err) { ERROR("Handshake: Unable to receive key"); return err; }

	err = set_key(queue);
	if (err) { ERROR("Handshake:  could not set key"); return err; }

	//gets reset on response in handle message
	hello(queue, deviceConfig->ota_success);

	err = blocking_send(queue, 18);
	if (0 > err) { ERROR("Hanshake: could not send hello message"); return err; }

	if (!event_loop(CoAPMessageType::HELLO, 2000))        // read the hello message from the server
	{
	  ERROR("Handshake: could not receive hello response");
	  return -1;
	}
	INFO("Hanshake: completed");
	return 0;
}






void remove_event_handlers(const char* event_name)
{
    if (NULL == event_name)
    {
        memset(event_handlers, 0, sizeof(event_handlers));
    }
    else
    {
        const int NUM_HANDLERS = sizeof(event_handlers) / sizeof(FilteringEventHandler);
        int dest = 0;
        for (int i = 0; i < NUM_HANDLERS; i++)
        {
          if (!strcmp(event_name, event_handlers[i].filter))
          {
              memset(&event_handlers[i], 0, sizeof(event_handlers[i]));
          }
          else
          {
              if (dest!=i) {
                memcpy(event_handlers+dest, event_handlers+i, sizeof(event_handlers[i]));
                memset(event_handlers+i, 0, sizeof(event_handlers[i]));
              }
              dest++;
          }
        }
    }
}




bool particleConnect(){
	if(deviceConfig->server_address_type == 1){
		//return client.connect(deviceConfig->server_address_domain,SPARK_SERVER_PORT);
    return pClient.connect("staging-device.spark.io",SPARK_SERVER_PORT);
		//return pClient.connect(IPAddress(192,168,0,111),SPARK_SERVER_PORT);
	}
	else{
		return pClient.connect(IPAddress(deviceConfig->server_address_ip),SPARK_SERVER_PORT);	
	}
}


//this connects to the configured wifi
bool wifiConnect(){
  WiFi.softAPdisconnect(false);
  WiFi.mode(WIFI_STA);
  if(deviceConfig->passcode[0] != '\0' && deviceConfig->channel > 0){
    WiFi.begin(deviceConfig->ssid,deviceConfig->passcode, deviceConfig->channel);
  }
  else if(deviceConfig->passcode[0] != '\0'){
    WiFi.begin(deviceConfig->ssid,deviceConfig->passcode);
  }
  else if(deviceConfig->channel > 0){
    WiFi.begin(deviceConfig->ssid, NULL, deviceConfig->channel);
  }
  else if (deviceConfig->ssid[0] != '\0'){
    WiFi.begin(deviceConfig->ssid);
  }
  else{
  	return false;
  }
  return true;
}

bool wifiConnected(){
  return (WiFi.status() == WL_CONNECTED);
}





bool wifiWaitForConnection(){ //returns false if it times out - I will later add code to jump to config rom in this case
  uint32_t timeoutTime = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED)
  {
    yield();
    //timeout after 15 seconds
    if(millis() > timeoutTime){
        return false;
      }
      delay(100);
  }
  return true;
}




bool readDeviceConfig(){
	noInterrupts();
	spi_flash_read(DEVICE_CONFIG_SECTOR * SECTOR_SIZE, reinterpret_cast<uint32_t*>(config_buffer), DEVICE_CONFIG_SIZE);

	if(deviceConfig->magic != DEVICE_MAGIC || deviceConfig->chksum != calc_device_chksum((uint8*)deviceConfig, (uint8*)&deviceConfig->chksum)){
	//load the backup and copy to main
	spi_flash_read(DEVICE_BACKUP_CONFIG_SECTOR * SECTOR_SIZE, reinterpret_cast<uint32_t*>(config_buffer), DEVICE_CONFIG_SIZE);
	spi_flash_erase_sector(DEVICE_CONFIG_SECTOR);
	spi_flash_write(DEVICE_CONFIG_SECTOR * SECTOR_SIZE, reinterpret_cast<uint32_t*>(config_buffer), DEVICE_CONFIG_SIZE);
	}
	interrupts();

	if(deviceConfig->magic != DEVICE_MAGIC || deviceConfig->chksum != calc_device_chksum((uint8*)deviceConfig, (uint8*)&deviceConfig->chksum)){
	    //TODO: reboot to config rom
	    while(1);
	    return false;
	}

	return true;
}









unsigned char next_token()
{
  return ++_token;
}

size_t time_request(unsigned char *buf)
{
    uint16_t msg_id = next_message_id();
    uint8_t token = next_token();
    return Messages::time_request(buf, msg_id, token);
}


bool send_time_request(void)
{
  if (updating)
  {
    return false;
  }

  size_t msglen = time_request(queue + 2);
  size_t wrapped_len = wrap(queue, msglen);
  last_chunk_millis = millis();

  return (0 <= blocking_send(queue, wrapped_len));
}

bool particle_handshake(){

  char buf[65];

  if(handshake()<0)
    return false;

  INFO("SEND EVENTS");
  if(deviceConfig->claim_code[0] != '\0')
    spark_send_event("spark/device/claim/code", buf, 60, PRIVATE, NULL);

  //send max size of rom
  ultoa(FLASH_MAX_SIZE, buf, 10);
  spark_send_event("spark/hardware/max_binary", buf, 60, PRIVATE, NULL);

  //send ota chunk size
  ultoa(OTA_CHUNK_SIZE, buf, 10);
  spark_send_event("spark/hardware/ota_chunk_size", buf, 60, PRIVATE, NULL);

  ///if we want to be able to get a system update we need to send that we are in safe more right now
  if (deviceConfig->system_version < OAK_SYSTEM_VERSION && bootConfig->current_rom != bootConfig->config_rom)
    spark_send_event("spark/device/safemode" "", "", 60, PRIVATE, NULL);

  #if defined(SPARK_SUBSYSTEM_EVENT_NAME)
    if (!HAL_core_subsystem_version(buf, sizeof (buf)) && *buf)
    {
        spark_send_event("spark/" SPARK_SUBSYSTEM_EVENT_NAME, buf, 60, PRIVATE, NULL);
    }
  #endif

  INFO("SEND SUBS");

  send_subscriptions();
  // important this comes at the end since it requires a response from the cloud.
  INFO("SEND TIME REQ");
  send_time_request();
  INFO("LOOP");
  event_loop();
  return true;
}




void rebootToUser(){
  bootConfig->current_rom = bootConfig->program_rom;
  ESP.restart();
  while(1);
}

void rebootToConfig(){
  bootConfig->current_rom = bootConfig->config_rom;
  ESP.restart();
  while(1);
}

String spark_deviceID(){
  return String(deviceConfig->device_id);
}

const char* CLAIM_EVENTS = "spark/device/claim/";
const char* RESET_EVENT = "spark/device/reset";
const char* OAK_RESET_EVENT = "oak/device/reset";
const char* OAK_RX_EVENT = "oak/device/stdin";

void SystemEvents(const char* name, const char* data)
{
    if (!strncmp(name, CLAIM_EVENTS, strlen(CLAIM_EVENTS))) {
        //mark as claimed
        deviceConfig->claim_code[0] != '\0';
        deviceConfig->claimed = 1;
        writeDeviceConfig();
    }
    if (!strcmp(name, RESET_EVENT)) {
        if (data && *data) {
            if (!strcmp("safe mode", data))
              return;
                //TODO System.enterSafeMode();
            else if (!strcmp("dfu", data))
              return;
                //TODO System.dfu(false);
            else if (!strcmp("reboot", data))
                ESP.reset();
        }
    }
    if (!strcmp(name, OAK_RESET_EVENT)) {
        if (data && *data) {
            if (!strcmp("config mode", data))
                rebootToConfig();
            else if (!strcmp("user mode", data))
                rebootToUser();
            else if (!strcmp("reboot", data))
                ESP.reset();
        }
    }
    if (!strcmp(name, OAK_RX_EVENT)) {
        if (data && *data) {
            /*
            while(*data != '\0'){
              // if buffer full, set the overflow flag and return
              uint8_t next = (spark_receive_buffer_tail + 1) % MAX_BUFF;
              if (next != spark_receive_buffer_head)
              {
                // save new data in buffer: tail points to where byte goes
                spark_receive_buffer[spark_receive_buffer_tail] = *data; // save new byte
                data++;
                spark_receive_buffer_tail = next;
              } 
              else 
              {
                spark_buffer_overflow = true;
                return;
              }
            }
             */
        }
    }
}


#define OAK_SYSTEM_ROM_4F616B 82

void oak_rom_init(){
  #ifndef OAK_SYSTEM_ROM_4F616B
    #define OAK_SYSTEM_ROM_4F616B 0
  #endif
  #ifdef OAK_SYSTEM_ROM_4F616B //DO NOT DEFINE THIS IN YOUR FILE OR IT MAY CORRUPT YOUR DEVICE
    if(OAK_SYSTEM_ROM_4F616B == 82 && deviceConfig->system_version < OAK_SYSTEM_VERSION){
      //this is a new system rom that we just booted into
      deviceConfig->system_version = OAK_SYSTEM_VERSION;
      memcpy(deviceConfig->version_string,OAK_SYSTEM_VERSION_STRING,sizeof(OAK_SYSTEM_VERSION_STRING));
      bootConfig->config_rom = bootConfig->current_rom;
      writeDeviceConfig();
      writeBootConfig();
      //go back to the user application
      rebootToUser();
    }
    else if(OAK_SYSTEM_ROM_4F616B != 82){
      //this is a new user rom, we have booted so set user rom to this
      if(bootConfig->program_rom != bootConfig->current_rom){ //if not already set
        bootConfig->program_rom = bootConfig->current_rom;
        writeBootConfig();
      }
    }
  #endif
}

//this should be called when the Particle library is inited 
void spark_initConfig(){
  if(spark_initialized)
    return;
  spark_initialized = true;
  Serial.println("INIT CONFIG");
  readDeviceConfig(); //will not return if valid device config does not exist, will reboot to config ROM - stubbed out for now
  readBootConfig();
  hex_decode(device_id,12,deviceConfig->device_id);
  oak_rom_init();
  spark_subscribe("spark", SystemEvents, NULL, ALL_DEVICES, NULL, NULL);
  spark_subscribe("oak", SystemEvents, NULL, MY_DEVICES, NULL, NULL);
}

bool spark_internal_connect(){
  if(!spark_initialized)
    spark_initConfig();
  if(!wifiConnected()){
    if(!wifiConnect()){
      Serial.println("WIFI");
      return false;
    }
    if(!wifiWaitForConnection()){
      Serial.println("WAIT");
      return false;
    }
  }
  if(!pClient.connected()){
    if(!particleConnect()){
      Serial.println("Particle");
      return false;
    }
    if(!particle_handshake()){
      Serial.println("SHAKE");
      return false;
    }

  }
  return true;
} 

uint8_t spark_failed_connects = 0;
uint32_t spark_last_failed_connect = 0;

bool spark_connect(){
  //connect with automatic back off
  //
  if(spark_failed_connects < 2 || 
    (spark_failed_connects < 5 && millis()-spark_last_failed_connect > 5000) || 
    millis()-spark_last_failed_connect > 30000 ){
    if(!spark_internal_connect()){
      spark_failed_connects++;
      spark_last_failed_connect = millis();
      return false;
    }
    else{
      spark_failed_connects = 0;
      return true;
    }
  }
  return false;
}

/*
void spark_send_tx(){
  if(spark_transmit_buffer_tail == spark_transmit_buffer_head)
    return;
  uint8_t buffer_length = (spark_transmit_buffer_tail + MAX_BUFF - spark_transmit_buffer_head) % MAX_BUFF;
  char buff[buffer_length];

  for(uint8_t b;b<buffer_length;b++){
    // Read from "head"
    buff[b] = spark_transmit_buffer[spark_transmit_buffer_head]; // grab next byte
    spark_transmit_buffer_head = (spark_transmit_buffer_head + 1) % MAX_BUFF;
  }

  //Particle.publish("oak/device/stdout", buff, 60, PRIVATE); 
  spark_send_event("oak/device/stdout", buff, 60, PRIVATE, NULL); 
}
*/


void spark_process()
{
  yield();
    if(spark_connected()){
      //spark_send_tx();
      if(!event_loop()){
        spark_disconnect();
        ERROR("EVENT LOOP FAIL!");
        return;
      }
    }
    else{
      spark_connect();
    }
    lastCloudEvent = millis();
}



