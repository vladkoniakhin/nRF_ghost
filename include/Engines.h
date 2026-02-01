#pragma once
#include "Common.h"

class IAttackEngine {
public:
    virtual ~IAttackEngine() = default;
    virtual void setup() = 0;
    virtual bool loop(StatusMessage& statusOut) = 0;
    virtual void stop() = 0;
};