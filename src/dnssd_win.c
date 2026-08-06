// M19: Windows implementation of the UxPlay dnssd interface, registering
// _raop._tcp / _airplay._tcp through the native Win32 DNS-SD API
// (DnsServiceRegister, dnsapi.dll) - Windows 10+ has a built-in mDNS
// responder, so no Bonjour install is required.
//
// Builds the same TXT records as the reference lib/dnssd.c.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windns.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnssd.h"
#include "dnssdint.h"
#include "global.h"
#include "utils.h"

#define MAX_TXT 24
#define MAX_ENTRY 192

typedef struct {
    wchar_t keys[MAX_TXT][64];
    wchar_t vals[MAX_TXT][MAX_ENTRY];
    PCWSTR keyPtr[MAX_TXT];
    PCWSTR valPtr[MAX_TXT];
    int count;
    PDNS_SERVICE_INSTANCE instance;
    int registered;
} svc_t;

struct dnssd_s {
    char* name;
    int name_len;
    char* hw_addr;
    int hw_addr_len;
    char* pk;
    uint32_t features1, features2;
    unsigned char require_pw;
    svc_t raop;
    svc_t airplay;
    wchar_t host[256]; // "<computername>.local"
};

static void widen(const char* s, wchar_t* out, int cap) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
}

static void txt_add(svc_t* svc, const char* key, const char* value) {
    if (svc->count >= MAX_TXT) return;
    widen(key, svc->keys[svc->count], 64);
    widen(value, svc->vals[svc->count], MAX_ENTRY);
    svc->keyPtr[svc->count] = svc->keys[svc->count];
    svc->valPtr[svc->count] = svc->vals[svc->count];
    svc->count++;
}

static void CALLBACK register_cb(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
    (void)ctx;
    if (status == ERROR_SUCCESS)
        printf("airplay: dns-sd registered %ls\n", inst ? inst->pszInstanceName : L"?");
    else
        fprintf(stderr, "airplay: dns-sd registration failed: %lu\n", status);
    if (inst) DnsServiceFreeInstance(inst);
}

static int do_register(dnssd_t* dnssd, svc_t* svc, const char* servname, const char* type,
                       unsigned short port) {
    wchar_t instanceName[512];
    char full[512];
    snprintf(full, sizeof(full), "%s.%s.local", servname, type);
    widen(full, instanceName, 512);

    svc->instance = DnsServiceConstructInstance(instanceName, dnssd->host, NULL, NULL, port, 0,
                                                0, svc->count, svc->keyPtr, svc->valPtr);
    if (!svc->instance) return -1;

    DNS_SERVICE_REGISTER_REQUEST req;
    memset(&req, 0, sizeof(req));
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;
    req.pServiceInstance = svc->instance;
    req.pRegisterCompletionCallback = register_cb;
    req.pQueryContext = dnssd;
    req.unicastEnabled = FALSE;

    DNS_STATUS st = DnsServiceRegister(&req, NULL);
    if (st != DNS_REQUEST_PENDING && st != ERROR_SUCCESS) {
        fprintf(stderr, "airplay: DnsServiceRegister(%s) failed: %lu\n", type, st);
        DnsServiceFreeInstance(svc->instance);
        svc->instance = NULL;
        return -1;
    }
    svc->registered = 1;
    return 0;
}

dnssd_t* dnssd_init(const char* name, int name_len, const char* hw_addr, int hw_addr_len,
                    int* error, int require_pw) {
    if (error) *error = DNSSD_ERROR_NOERROR;
    dnssd_t* d = (dnssd_t*)calloc(1, sizeof(dnssd_t));
    if (!d) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    d->require_pw = (unsigned char)require_pw;
    char* end;
    d->features1 = (uint32_t)strtoul(FEATURES_1, &end, 16);
    d->features2 = (uint32_t)strtoul(FEATURES_2, &end, 16);
    d->name_len = name_len;
    d->name = (char*)calloc(1, name_len + 1);
    d->hw_addr_len = hw_addr_len;
    d->hw_addr = (char*)calloc(1, hw_addr_len);
    memcpy(d->name, name, name_len);
    memcpy(d->hw_addr, hw_addr, hw_addr_len);

    wchar_t comp[128];
    DWORD n = 128;
    if (GetComputerNameW(comp, &n)) {
        _snwprintf(d->host, 256, L"%ls.local", comp);
    } else {
        wcscpy(d->host, L"vkrender.local");
    }
    return d;
}

void dnssd_destroy(dnssd_t* d) {
    if (d) {
        free(d->name);
        free(d->hw_addr);
        free(d);
    }
}

int dnssd_register_raop(dnssd_t* d, unsigned short port) {
    char features[22];
    snprintf(features, sizeof(features), "0x%X,0x%X", d->features1, d->features2);
    svc_t* s = &d->raop;
    s->count = 0;
    txt_add(s, "ch", RAOP_CH);
    txt_add(s, "cn", RAOP_CN);
    txt_add(s, "da", RAOP_DA);
    txt_add(s, "et", RAOP_ET);
    txt_add(s, "vv", RAOP_VV);
    txt_add(s, "ft", features);
    txt_add(s, "am", GLOBAL_MODEL);
    txt_add(s, "md", RAOP_MD);
    txt_add(s, "rhd", RAOP_RHD);
    txt_add(s, "pw", d->require_pw ? "true" : "false");
    txt_add(s, "sr", RAOP_SR);
    txt_add(s, "ss", RAOP_SS);
    txt_add(s, "sv", RAOP_SV);
    txt_add(s, "tp", RAOP_TP);
    txt_add(s, "txtvers", RAOP_TXTVERS);
    txt_add(s, "sf", RAOP_SF);
    txt_add(s, "vs", RAOP_VS);
    txt_add(s, "vn", RAOP_VN);
    txt_add(s, "pk", d->pk ? d->pk : "");

    char servname[256];
    if (utils_hwaddr_raop(servname, sizeof(servname), d->hw_addr, d->hw_addr_len) < 0) return -1;
    strncat(servname, "@", sizeof(servname) - strlen(servname) - 1);
    strncat(servname, d->name, sizeof(servname) - strlen(servname) - 1);
    return do_register(d, s, servname, "_raop._tcp", port);
}

int dnssd_register_airplay(dnssd_t* d, unsigned short port) {
    char device_id[3 * MAX_HWADDR_LEN];
    char features[22];
    snprintf(features, sizeof(features), "0x%X,0x%X", d->features1, d->features2);
    if (utils_hwaddr_airplay(device_id, sizeof(device_id), d->hw_addr, d->hw_addr_len) < 0)
        return -1;
    svc_t* s = &d->airplay;
    s->count = 0;
    txt_add(s, "deviceid", device_id);
    txt_add(s, "features", features);
    txt_add(s, "flags", AIRPLAY_FLAGS);
    txt_add(s, "model", GLOBAL_MODEL);
    txt_add(s, "pk", d->pk ? d->pk : "");
    txt_add(s, "pw", d->require_pw ? "true" : "false");
    txt_add(s, "pi", AIRPLAY_PI);
    txt_add(s, "srcvers", AIRPLAY_SRCVERS);
    txt_add(s, "vv", AIRPLAY_VV);
    return do_register(d, s, d->name, "_airplay._tcp", port);
}

static void unregister(svc_t* s) {
    if (s->registered && s->instance) {
        DNS_SERVICE_REGISTER_REQUEST req;
        memset(&req, 0, sizeof(req));
        req.Version = DNS_QUERY_REQUEST_VERSION1;
        req.pServiceInstance = s->instance;
        req.pRegisterCompletionCallback = register_cb;
        DnsServiceDeRegister(&req, NULL);
    }
    s->registered = 0;
    s->instance = NULL;
}

void dnssd_unregister_raop(dnssd_t* d) { unregister(&d->raop); }
void dnssd_unregister_airplay(dnssd_t* d) { unregister(&d->airplay); }

const char* dnssd_get_airplay_txt(dnssd_t* d, int* length) {
    *length = 0;
    (void)d;
    return "";
}
const char* dnssd_get_name(dnssd_t* d, int* length) {
    *length = d->name_len;
    return d->name;
}
const char* dnssd_get_hw_addr(dnssd_t* d, int* length) {
    *length = d->hw_addr_len;
    return d->hw_addr;
}
uint64_t dnssd_get_airplay_features(dnssd_t* d) {
    return (((uint64_t)d->features2) << 32) + (uint64_t)d->features1;
}
void dnssd_set_pk(dnssd_t* d, char* pk_str) { d->pk = pk_str; }
void dnssd_set_airplay_features(dnssd_t* d, int bit, int val) {
    uint32_t mask, *f;
    if (bit < 0 || bit > 63 || val < 0 || val > 1) return;
    if (bit >= 32) {
        mask = 0x1u << (bit - 32);
        f = &d->features2;
    } else {
        mask = 0x1u << bit;
        f = &d->features1;
    }
    if (val) *f |= mask;
    else *f &= ~mask;
}

#endif // _WIN32
