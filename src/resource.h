#pragma once

// Версия программы. Используется в ресурсе версии exe.
#define KA_VERSION_MAJOR 2
#define KA_VERSION_MINOR 4
#define KA_VERSION_PATCH 0
#define KA_VERSION_STRING "2.4.0"

// Иконки. Первая по номеру иконка становится иконкой exe в Проводнике
// и на ярлыке, поэтому первой идёт зелёная иконка с открытым глазом.
#define IDI_KEEPAWAKE_ON  101
#define IDI_KEEPAWAKE_OFF 102
#define IDI_KEEPAWAKE_ON_DISPLAY 103  // синий открытый глаз: сон и экран

// Окно выбора времени «До времени…».
#define IDD_TIME_PICKER 200
#define IDC_TIME_LABEL  201
#define IDC_TIME_VALUE  202
#define IDC_TIME_SPIN   203
