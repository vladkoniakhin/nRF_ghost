import os
Import("env")

# Функция, которая создает единый бинарник
def make_merged_bin(source, target, env):
    # Пути к файлам
    bootloader = os.path.join("$BUILD_DIR", "bootloader.bin")
    partitions = os.path.join("$BUILD_DIR", "partitions.bin")
    boot_app0 = os.path.join("$PROJECT_PACKAGES_DIR", "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
    firmware = os.path.join("$BUILD_DIR", "${PROGNAME}.bin")
    
    # Имя выходного файла
    merged_bin = os.path.join("$BUILD_DIR", "firmware-merged.bin")

    # Команда для esptool
    # Смещения: 0x1000(bootloader), 0x8000(partitions), 0xE000(boot_app0), 0x10000(firmware)
    cmd = (
        f'"$PYTHONEXE" "$PROJECT_PACKAGES_DIR/tool-esptoolpy/esptool.py" '
        f'--chip esp32 merge_bin '
        f'-o "{merged_bin}" '
        f'--flash_mode dio --flash_freq 40m --flash_size 4MB '
        f'0x1000 "{bootloader}" '
        f'0x8000 "{partitions}" '
        f'0xe000 "{boot_app0}" '
        f'0x10000 "{firmware}"'
    )

    print("Generating single binary...")
    env.Execute(cmd)

# Добавляем этот скрипт как пост-экшен (выполнится после успешной сборки)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", make_merged_bin)