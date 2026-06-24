cd "/Users/dirk/Library/Mobile Documents/com~apple~CloudDocs/Documents/Arduino/PylontechMonitoring" && cp partitions/pylontech_ota_spiffs_16M.csv partitions.csv && arduino-cli compile --fqbn "esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=16M,PSRAM=opi" --libraries libraries/ --output-dir build/ .

