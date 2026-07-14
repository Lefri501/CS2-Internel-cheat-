#include "log.h"
#include "../../interfaces/interfaces.h"

// Temporary, will implement much more beautiful logging code wise (also I'm not really sorry for the watermark formatting :3)

void Logger::Log(const char* text, LogType logType) {
    std::string message;
    Color color(255, 255, 255, 255);

    switch (logType) {
        case LogType::Info:
            message = "\n[*] ";
            break;
        case LogType::Warning:
            message = "\n[?] ";
            color = Color(255, 222, 105, 255);
            break;
        case LogType::Error:
            message = "\n[!] ";
            color = Color(255, 92, 87, 255);
            break;
        case LogType::None:
            message = "\n";
    }

    message += text;
    I::ConColorMsg(color, message.c_str());
}

void Logger::Watermark() {
    const char* watermark = "\n /$$$$$$$$                                /$$           /$$      /$$                              \n|__  $$__/                               | $$          | $$  /$ | $$                              \n   | $$  /$$$$$$  /$$$$$$/$$$$   /$$$$$$ | $$  /$$$$$$ | $$ /$$$| $$  /$$$$$$   /$$$$$$   /$$$$$$ \n   | $$ /$$__  $$| $$_  $$_  $$ /$$__  $$| $$ /$$__  $$| $$/$$ $$ $$ |____  $$ /$$__  $$ /$$__  $$\n   | $$| $$$$$$$$| $$ \\ $$ \\ $$| $$  \\ $$| $$| $$$$$$$$| $$$$_  $$$$  /$$$$$$$| $$  \\__/| $$$$$$$$\n   | $$| $$_____/| $$ | $$ | $$| $$  | $$| $$| $$_____/| $$$/ \\  $$$ /$$__  $$| $$      | $$_____/\n   | $$|  $$$$$$$| $$ | $$ | $$| $$$$$$$/| $$|  $$$$$$$| $$/   \\  $$|  $$$$$$$| $$      |  $$$$$$$\n   |__/ \\_______/|__/ |__/ |__/| $$____/ |__/ \\_______/|__/     \\__/ \\_______/|__/       \\_______/\n                               | $$                                                               \n                               | $$                                                               \n                               |__/                                                               \n";
    I::ConColorMsg(Color(152, 52, 224, 255), watermark);
}
