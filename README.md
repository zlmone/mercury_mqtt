Утилита для получения данных со счётчика Меркурий (3 фазы) по RS485 и отправки данных в MQTT

Для сборки необходимы пакеты gcc, mske и библиотека mosquitto, утсновка на debian, ububtu - sudo apt install libmosquitto-dev build-essential

Настройка MQTT сервера в файле mercury236mqtt.c

#define MQTT_SERVER	"10.32.0.5" - IP адрес MQTT-сервера

#define MQTT_USER	"" - Имя пользователя MQTT

#define MQTT_PASSWORD	"" - пароль

Для сборки - make

Запуск для проверки связи: ./mercury236mqtt /dev/ttyUSB0 --debug

Запуск в фоне: nohup ./mercury236mqtt /dev/ttyUSB0 &

либо

./mercury_daemon.sh - Убивает старый процесс и запускает в фоне новый.

Если сервер MQTT перезапускается, то утилита может потерять связь с ним, необходимо перезапустить её. Можно поставить mercury_daemon.sh в cron каждый час, день.\

Также есть настройки по частоте отправки данных в MQTT:

#define LOOP_DELAY	5 //seconds - Задержка между опросами счётчика в секундах

#define SKIP_SEND	10 - количество опросов счётчика перед отправкой.

Каждые 60 секунд:

#define LOOP_DELAY	60

#define SKIP_SEND	1

При высокой частоте опроса (менее 5 секунд) счётчик не успевает выдавать все данные.
