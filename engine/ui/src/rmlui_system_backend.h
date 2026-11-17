/// @file rmlui_system_backend.h
/// @brief RmlUi SystemInterface implementation bridging logging and timing.

#pragma once

#include <RmlUi/Core/SystemInterface.h>

namespace eng::ui {

class AnteraySystemInterface final : public Rml::SystemInterface {
public:
    AnteraySystemInterface();
    ~AnteraySystemInterface() override = default;

    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

    void advanceTime(double deltaSeconds);

private:
    double elapsedTime_{0.0};
};

} // namespace eng::ui
