#ifndef SPARK_APPLICATION_COMMAND_COMMAND_SENDER_H
#define SPARK_APPLICATION_COMMAND_COMMAND_SENDER_H

#include <format>
#include <string>

namespace spark {

// Abstract command sender, platform-independent.
// The application layer uses this interface to communicate with whoever
// invoked a command (console, player, etc.) without depending on Endstone.
class CommandSender {
public:
    virtual ~CommandSender() = default;

    virtual std::string getName() const = 0;
    virtual bool isPlayer() const = 0;

    template <typename... Args>
    void sendMessage(const std::string &fmt, Args &&...args)
    {
        if constexpr (sizeof...(Args) == 0) {
            sendImpl(fmt);
        } else {
            sendImpl(std::vformat(fmt, std::make_format_args(args...)));
        }
    }

    template <typename... Args>
    void sendErrorMessage(const std::string &fmt, Args &&...args)
    {
        if constexpr (sizeof...(Args) == 0) {
            errorImpl(fmt);
        } else {
            errorImpl(std::vformat(fmt, std::make_format_args(args...)));
        }
    }

private:
    virtual void sendImpl(const std::string &message) = 0;
    virtual void errorImpl(const std::string &message) = 0;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_COMMAND_COMMAND_SENDER_H
