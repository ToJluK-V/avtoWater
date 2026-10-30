#pragma once

// Скопируйте в secrets.h и впишите свои значения. secrets.h в git не попадает.
#define WIFI_SSID      "your-wifi-name"
#define WIFI_PASSWORD  "your-wifi-password"

// Хост сервера на Render без https:// и без слеша.
#define SERVER_HOST    "your-app.onrender.com"

// Уникальное имя платы: буквы, цифры, '-' и '_' (до 32 символов).
#define DEVICE_ID      "plant1"

// Должен совпадать с переменной DEVICE_KEY на сервере.
#define DEVICE_KEY     "REPLACE_WITH_LONG_RANDOM_KEY"
