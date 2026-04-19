# 16 AT Commands for HTTP(S)

> A76XX Series — AT Command Manual V1.09

---

## 16.1 Overview of AT Commands for HTTP(S)

| Command | Description |
|---|---|
| AT+HTTPINIT | Start HTTP service |
| AT+HTTPTERM | Stop HTTP Service |
| AT+HTTPPARA | Set HTTP Parameters value |
| AT+HTTPACTION | HTTP Method Action |
| AT+HTTPHEAD | Read the HTTP Header Information of Server Response |
| AT+HTTPREAD | Read the response information of HTTP Server |
| AT+HTTPDATA | Input HTTP Data |
| AT+HTTPPOSTFILE | Send HTTP Request to HTTP(S)server by File |
| AT+HTTPREADFILE | Receive HTTP Response Content to a file |

---

## 16.2 Detailed Description of AT Commands for HTTP(S)

### 16.2.1 AT+HTTPINIT — Start HTTP Service

AT+HTTPINIT is used to start HTTP service by activating PDP context. You must execute AT+HTTPINIT before any other HTTP related operations.

| AT+HTTPINIT — Start HTTP Service | |
|---|---|
| Test Command: `AT+HTTPINIT=?` | Response: `OK` |
| Execute Command: `AT+HTTPINIT` | Response:<br>1) If start HTTP service successfully: `OK`<br>2) If failed: `ERROR` |
| Parameter Saving Mode | - |
| Max Response Time | 120000ms |
| Reference | - |

**Defined Values**

| Parameter | Description |
|---|---|
| `<err>` | The type of error — please refer to Section 16.4 |

**Examples**
```
AT+HTTPINIT
OK
```

---

### 16.2.2 AT+HTTPTERM — Stop HTTP Service

AT+HTTPTERM is used to stop HTTP service.

| AT+HTTPTERM — Stop HTTP Service | |
|---|---|
| Test Command: `AT+HTTPTERM=?` | Response: `OK` |
| Execute Command: `AT+HTTPTERM` | Response:<br>1) If stop HTTP service successfully: `OK`<br>2) If failed: `ERROR` |
| Parameter Saving Mode | - |
| Max Response Time | 120000ms |
| Reference | - |

**Examples**
```
AT+HTTPTERM
OK
```

---

### 16.2.3 AT+HTTPPARA — Set HTTP Parameters value

AT+HTTPPARA is used to set HTTP parameters value. When you want to access a HTTP server, you should input `<value>` like `http://'server'/'path':'tcpPort'`. In addition, `https://'server'/'path':'tcpPort'` is used to access a HTTPS server.

| AT+HTTPPARA — Set HTTP Parameters value | |
|---|---|
| Test Command: `AT+HTTPPARA=?` | Response: `OK` |
| Write Command: `AT+HTTPPARA="URL",<url>` | Response:<br>1) If parameter format is right: `OK`<br>2) If parameter format is not right or other errors occur: `ERROR` |
| Write Command: `AT+HTTPPARA="CONNECTTO",<conn_timeout>` | Response:<br>1) If parameter format is right: `OK`<br>2) If parameter format is not right or other errors occur: `ERROR` |
| Write Command: `AT+HTTPPARA="RECVTO",<recv_timeout>` | Response:<br>1) If parameter format is right: `OK`<br>2) If parameter format is not right or other errors occur: `ERROR` |
| Write Command: `AT+HTTPPARA="CONTENT",<content_type>` | Response:<br>1) If parameter format is right: `OK`<br>2) If parameter format is not right or other errors occur: `ERROR` |
| Write Command: `AT+HTTPPARA="ACCEPT",<accept-type>` | Response:<br>1) If parameter format is right: `OK`<br>2) If parameter format is not right or other errors occur: `ERROR` |
| Write Command: `AT+HTTPPARA="SSLCFG",<sslcfg_id>` | Response:<br>1) If parameter format is right: `OK`<br>2) If parameter format is not right or other errors occur: `ERROR` |
| Write Command: `AT+HTTPPARA="USERDATA",<user_data>` | Response:<br>1) If parameter format is right: `OK`<br>2) If parameter format is not right or other errors occur: `ERROR` |
| Write Command: `AT+HTTPPARA="READMODE",<readmode>` | Response:<br>1) If parameter format is right: `OK`<br>2) If parameter format is not right or other errors occur: `ERROR` |
| Parameter Saving Mode | - |
| Max Response Time | 120000ms |
| Reference | - |

**Defined Values**

| Parameter | Description |
|---|---|
| `<url>` | URL of network resource. String, start with `"http://"` or `"https://"`<br>a) `http://'server':'tcpPort'/'path'`<br>b) `https://'server':'tcpPort'/'path'`<br>`"server"` — DNS domain name or IP address<br>`"path"` — path to a file or directory of a server<br>`"tcpPort"` — HTTP default value is 80, HTTPS default value is 443 (can be omitted) |
| `<conn_timeout>` | Timeout for accessing server. Numeric type, range is 20–120s, default is 120s. |
| `<recv_timeout>` | Timeout for receiving data from server. Numeric type, range is 2s–120s, default is 20s. |
| `<content_type>` | This is for HTTP "Content-Type" tag. String type, max length is 256, default is `"text/plain"`. |
| `<accept-type>` | This is for HTTP "Accept-type" tag. String type, max length is 256, default is `"*/*"`. |
| `<sslcfg_id>` | This is setting SSL context id. Numeric type, range is 0–9. Default is 0. Please refer to Chapter 19 of this document. |
| `<user_data>` | The customized HTTP header information. String type, max length is 256. |
| `<readmode>` | For HTTPREAD. Numeric type, can be set to 0 or 1. If set to 1, you can read the response content data from the same position repeatedly. The limit is that the size of HTTP server response content should be shorter than 1M. Default is 0. |

> **NOTE:** When you want to use content-type `multipart/form-data` to transfer data, you should set `AT+HTTPPARA="CONTENT","multipart/form-data"`. The boundary header will be constructed automatically.

**Examples**
```
AT+HTTPPARA="URL","http://www.baidu.com"
OK
```

---

### 16.2.4 AT+HTTPACTION — HTTP Method Action

AT+HTTPACTION is used to perform a HTTP Method. You can use HTTPACTION to send a GET/POST request to a HTTP/HTTPS server.

| AT+HTTPACTION — HTTP Method Action | |
|---|---|
| Test Command: `AT+HTTPACTION=?` | Response: `+HTTPACTION: (0-4)`<br>`OK` |
| Write Command: `AT+HTTPACTION=<method>` | Response:<br>1) If parameter format is right: `OK`<br>`+HTTPACTION: <method>,<statuscode>,<datalen>`<br>2) If parameter format is right but server connected unsuccessfully: `OK`<br>`+HTTPACTION: <method>,<errcode>,<datalen>`<br>3) If parameter format is not right or other errors occur: `ERROR` |
| Parameter Saving Mode | - |
| Max Response Time | 120000ms |
| Reference | - |

**Defined Values**

| Parameter | Description |
|---|---|
| `<method>` | HTTP method specification:<br>`0` — GET<br>`1` — POST<br>`2` — HEAD<br>`3` — DELETE<br>`4` — PUT |
| `<statuscode>` | Please refer to the end of this chapter |
| `<datalen>` | The length of data received |

**Examples**
```
AT+HTTPACTION=?
+HTTPACTION: (0-4)
OK

AT+HTTPACTION=0
OK
+HTTPACTION: 0,200,104220
```

---

### 16.2.5 AT+HTTPHEAD — Read the HTTP Header Information of Server Response

AT+HTTPHEAD is used to read the HTTP header information of server response when the module receives the response data from server.

| AT+HTTPHEAD — Read the HTTP Header Information of Server Response | |
|---|---|
| Test Command: `AT+HTTPHEAD=?` | Response: `OK` |
| Execute Command: `AT+HTTPHEAD` | Response:<br>1) If read the header information successfully:<br>`+HTTPHEAD: <data_len>`<br>`<data>`<br>`OK`<br>2) If read failed: `ERROR` |
| Parameter Saving Mode | - |
| Max Response Time | 120000ms |
| Reference | - |

**Defined Values**

| Parameter | Description |
|---|---|
| `<data_len>` | The length of HTTP header |
| `<data>` | The header information of HTTP response |

**Examples**
```
AT+HTTPHEAD
+HTTPHEAD: 653
HTTP/1.1 200 OK
Content-Type: text/html
Connection: keep-alive
...
OK
```

---

### 16.2.6 AT+HTTPREAD — Read the response information of HTTP Server

After sending HTTP(S) GET/POST requests, you can retrieve HTTP(S) response information from the server via UART/USB port using AT+HTTPREAD. When the `<datalen>` of `+HTTPACTION: <method>,<statuscode>,<datalen>` is not equal to 0, you can execute `AT+HTTPREAD=<start_offset>,<byte_size>` to read data to port. If `<byte_size>` is set greater than the size of data saved in buffer, all data in cache will be output to port.

| AT+HTTPREAD — Read the response information of HTTP Server | |
|---|---|
| Test Command: `AT+HTTPREAD=?` | Response: `OK` |
| Read Command: `AT+HTTPREAD?` | Response:<br>1) If check successfully: `+HTTPREAD: LEN,<len>`<br>`OK`<br>2) If failed: `ERROR` |
| Write Command: `AT+HTTPREAD=[<start_offset>,]<byte_size>` | Response:<br>1) If read the response info successfully:<br>`OK`<br>`+HTTPREAD: <data_len>`<br>`<data>`<br>`+HTTPREAD: 0`<br>(If `<byte_size>` is bigger than data received, only actual data size is returned)<br>2) If read failed: `ERROR` |
| Parameter Saving Mode | - |
| Max Response Time | 120000ms |
| Reference | - |

**Defined Values**

| Parameter | Description |
|---|---|
| `<start_offset>` | The start position of reading |
| `<byte_size>` | The length of data to read |
| `<data_len>` | The actual length of read data |
| `<data>` | Response content from HTTP server |
| `<len>` | Total size of data saved in buffer |

**Examples**
```
AT+HTTPREAD?
+HTTPREAD: LEN,22505
OK

AT+HTTPREAD=0,500
OK
+HTTPREAD: 500
...
+HTTPREAD: 0
```

> **NOTE:** The response content received from server will be saved in cache, and would not be cleaned up by AT+HTTPREAD. Due to the max size of protocol stack being 64K bytes (CAT4 module is 10K bytes), when the total size of the data from server is bigger than that and `READMODE` is 0, you should read the data quickly, or you will fail to read it.

---

### 16.2.7 AT+HTTPDATA — Input HTTP Data

You can use AT+HTTPDATA to input data to post when sending a HTTP/HTTPS POST request.

| AT+HTTPDATA — Input HTTP Data | |
|---|---|
| Test Command: `AT+HTTPDATA=?` | Response: `OK` |
| Write Command: `AT+HTTPDATA=<size>,<time>` | Response:<br>1) If parameter format is right:<br>`DOWNLOAD`<br>`<input data here>`<br>When the total size of inputted data reaches `<size>`, TA will report: `OK`<br>Otherwise the serial port will be blocked.<br>2) If parameter format is wrong or other errors occur: `ERROR` |
| Parameter Saving Mode | - |
| Max Response Time | - |
| Reference | - |

**Defined Values**

| Parameter | Description |
|---|---|
| `<size>` | Size in bytes of the data to post. Range is 1–153600 bytes. |
| `<time>` | Maximum time in seconds to input data. Range is 10–65535. |

**Examples**
```
AT+HTTPDATA=18,1000
DOWNLOAD
Message=helloworld
OK
```

---

### 16.2.8 AT+HTTPPOSTFILE — Send HTTP Request to HTTP(S) server by File

You can also send an HTTP request from a file via AT+HTTPPOSTFILE. The URL must be set by AT+HTTPPARA before executing AT+HTTPPOSTFILE. The parameter `<path>` sets the file directory. When the modem has received a response from the HTTP server, it will report the following URC:
`+HTTPPOSTFILE: <statuscode>,<datalen>`

| AT+HTTPPOSTFILE — Send HTTP Request to HTTP(S) server by File | |
|---|---|
| Test Command: `AT+HTTPPOSTFILE=?` | Response: `+HTTPPOSTFILE: <filename>[,(1-2)[,(0-4)[,(0-1)]]]`<br>`OK` |
| Write Command: `AT+HTTPPOSTFILE=<filename>[,<path>[,<method>[,<send_header>]]]` | Response:<br>1) If format right and server connected successfully:<br>a) If `<method>` is valid: `OK`<br>`+HTTPPOSTFILE: <method>,<statuscode>,<datalen>`<br>b) If `<method>` is ignored: `OK`<br>`+HTTPPOSTFILE: <statuscode>,<datalen>`<br>2) If format right but server connected unsuccessfully:<br>a) If `<method>` is valid: `OK`<br>`+HTTPPOSTFILE: <method>,<errcode>,0`<br>b) If `<method>` is ignored: `OK`<br>`+HTTPPOSTFILE: <errcode>,0`<br>3) If format is not right or any other error: `ERROR` |
| Parameter Saving Mode | - |
| Max Response Time | - |
| Reference | - |

**Defined Values**

| Parameter | Description |
|---|---|
| `<filename>` | String type, filename, max length is 112 bytes. |
| `<path>` | Directory where the sent file is saved. Numeric type, range 1–2.<br>`1` — C:/ (local storage)<br>`2` — D:/ (SD card) |
| `<method>` | HTTP method specification:<br>`0` — GET<br>`1` — POST<br>`2` — HEAD<br>`3` — DELETE<br>`4` — PUT<br>If not provided, value is taken from the post file. |
| `<send_header>` | Send file as HTTP header and Body or only as Body. Numeric type, range 0–1, default 0.<br>`0` — Send file as Body<br>`1` — Send file as HTTP header and body |
| `<statuscode>` | Please refer to the end of this chapter |
| `<datalen>` | The length of data received |

**Examples**
```
AT+HTTPPOSTFILE=?
+HTTPPOSTFILE: <filename>[,(1-2)[,(0-4)[,(0-1)]]]
OK

AT+HTTPPOSTFILE="getbaidu.txt",1
OK
+HTTPPOSTFILE: 200,14615

AT+HTTPPOSTFILE="getbaidu.txt",1,1,1
OK
+HTTPPOSTFILE: 1,200,14615
```

---

### 16.2.9 AT+HTTPREADFILE — Receive HTTP Response Content to a file

After executing AT+HTTPACTION or AT+HTTPPOSTFILE, you can receive the HTTP server response content to a file via AT+HTTPREADFILE. Before executing AT+HTTPREADFILE, either `+HTTPACTION: <method>,<statuscode>,<datalen>` or `+HTTPPOSTFILE: <statuscode>,<datalen>` must have been received. The parameter `<path>` sets the directory to save the file. If `<path>` is omitted, the file will be saved to local storage.

| AT+HTTPREADFILE — Receive HTTP Response Content to a File | |
|---|---|
| Test Command: `AT+HTTPREADFILE=?` | Response: `+HTTPREADFILE: <filename>[,(1-2)]`<br>`OK` |
| Write Command: `AT+HTTPREADFILE=<filename>[,<path>]` | Response:<br>1) If parameter format is right: `OK`<br>`+HTTPREADFILE: <errcode>`<br>2) If failed: `OK`<br>`+HTTPREADFILE: <errcode>`<br>3) If parameter format is not right or any other error: `ERROR` |
| Parameter Saving Mode | - |
| Max Response Time | - |
| Reference | - |

**Defined Values**

| Parameter | Description |
|---|---|
| `<filename>` | String type, filename, max length is 112 bytes. |
| `<path>` | Directory where the read file is saved. Numeric type, range 1–2.<br>`1` — C:/ (local storage)<br>`2` — D:/ (SD card) |

**Examples**
```
AT+HTTPREADFILE=?
+HTTPREADFILE: <filename>[,(1-2)]
OK

AT+HTTPREADFILE="readbaidu.dat"
OK
+HTTPREADFILE: 0
```

---

## 16.3 Command Result Codes

### 16.3.1 Description of `<statuscode>`

| `<statuscode>` | Description |
|---|---|
| 100 | Continue |
| 101 | Switching Protocols |
| 200 | OK |
| 201 | Created |
| 202 | Accepted |
| 203 | Non-Authoritative Information |
| 204 | No Content |
| 205 | Reset Content |
| 206 | Partial Content |
| 300 | Multiple Choices |
| 301 | Moved Permanently |
| 302 | Found |
| 303 | See Other |
| 304 | Not Modified |
| 305 | Use Proxy |
| 307 | Temporary Redirect |
| 400 | Bad Request |
| 401 | Unauthorized |
| 402 | Payment Required |
| 403 | Forbidden |
| 404 | Not Found |
| 405 | Method Not Allowed |
| 406 | Not Acceptable |
| 407 | Proxy Authentication Required |
| 408 | Request Timeout |
| 409 | Conflict |
| 410 | Gone |
| 411 | Length Required |
| 412 | Precondition Failed |
| 413 | Request Entity Too Large |
| 414 | Request-URI Too Large |
| 415 | Unsupported Media Type |
| 416 | Requested range not satisfiable |
| 417 | Expectation Failed |
| 500 | Internal Server Error |
| 501 | Not Implemented |
| 502 | Bad Gateway |
| 503 | Service Unavailable |
| 504 | Gateway timeout |
| 505 | HTTP Version not supported |
| 600 | Not HTTP PDU |
| 601 | Network Error |
| 602 | No memory |
| 603 | DNS Error |
| 604 | Stack Busy |

---

### 16.3.2 Description of `<errcode>`

| `<errcode>` | Meaning |
|---|---|
| 0 | Success |
| 701 | Alert state |
| 702 | Unknown error |
| 703 | Busy |
| 704 | Connection closed error |
| 705 | Timeout |
| 706 | Receive/send socket data failed |
| 707 | File not exists or other memory error |
| 708 | Invalid parameter |
| 709 | Network error |
| 710 | Start a new SSL session failed |
| 711 | Wrong state |
| 712 | Failed to create socket |
| 713 | Get DNS failed |
| 714 | Connect socket failed |
| 715 | Handshake failed |
| 716 | Close socket failed |
| 717 | No network error |
| 718 | Send data timeout |
| 719 | CA missed |

---

## 16.4 Unsolicited Result Codes

| URC | Description |
|---|---|
| `+HTTP_PEER_CLOSED` | Notification message. While received, it means the connection has been closed by server. |
| `+HTTP_NONET_EVENT` | Notification message. While received, it means the network is now unavailable. |
