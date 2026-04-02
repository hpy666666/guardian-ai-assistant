#ifndef BOARD_H
#define BOARD_H

#include <string>

// ── 前向声明 ──────────────────────────────────────────────────
class AudioCodec;

// ── 极简桩类（我们的板子不需要 LED/Display/Camera） ──────────

class Led {
public:
    virtual ~Led() = default;
};
class NoLed : public Led {};

class Backlight {
public:
    virtual ~Backlight() = default;
    virtual int brightness() const { return 100; }
};

class Display {
public:
    virtual ~Display() = default;
    virtual int height() const { return 0; }
    virtual void ShowNotification(const char* msg, int duration_ms = 3000) {}
    virtual std::string GetTheme() const { return "light"; }
};
class NoDisplay : public Display {};

class Camera {
public:
    virtual ~Camera() = default;
};

// ── Board 基类（去掉所有 xiaozhi NetworkInterface 依赖） ──────
void* create_board();

class Board {
private:
    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;

protected:
    Board();
    std::string GenerateUuid();
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& temp);
    virtual Display* GetDisplay();
    virtual Camera* GetCamera();

    // WiFi 启动（子类实现）
    virtual void StartNetwork() = 0;
    virtual const char* GetNetworkStateIcon() = 0;
    virtual void SetPowerSaveMode(bool enabled) = 0;

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging);
    virtual std::string GetJson();
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
