# A76XX Series — AT Command Manual V1.09

---

## 14.2.3 AT+CNTP — Update system time

This command is used to update system time with NTP server.

### AT+CNTP — Update system time

**Test Command**

```
AT+CNTP=?
```

**Response**

```
+CNTP: "HOST",(-96~96)

OK
```

---

**Read Command**

```
AT+CNTP?
```

**Response**

```
+CNTP: <host>,<timezone>

OK
```

---

**Write Command**

```
AT+CNTP=<host>[,<timezone>]
```

**Response**

1. If successfully:

```
OK
```

2. If failed:

```
ERROR
```

---

**Execute Command**

```
AT+CNTP
```

**Response**

1. If successfully:

```
OK

+CNTP: <err>
```

2. If failed:

```
ERROR
```

| Parameter Saving Mode | — |
|-----------------------|---|
| Max Response Time     | — |
| Reference             |   |

### Defined Values

| Parameter    | Description                                              |
|--------------|----------------------------------------------------------|
| `<host>`     | NTP server address, length is 0–255.                     |
| `<timezone>` | Local time zone, the range is (−96 to 96), default value is 32. |

### Examples

```
AT+CNTP="120.25.115.20",32
OK

AT+CNTP
OK

+CNTP: 0
```

---

## 14.3 Command Result Codes

### 14.3.2 Description of `<err>` of NTP

| `<err>` | Description                    |
|---------|--------------------------------|
| 0       | Operation succeeded            |
| 1       | Unknown error                  |
| 2       | Wrong parameter                |
| 3       | Wrong date and time calculated |
| 4       | Network error                  |
| 5       | Time zone error                |
| 6       | Time out error                 |

---

*Source: SIMCom A76XX Series AT Command Manual V1.09, pages 311–313*
