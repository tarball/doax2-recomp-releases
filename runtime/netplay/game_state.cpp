#include "netplay/game_state.h"

#include <optional>

namespace netplay
{
    namespace
    {
        std::optional<GameState>& Registered()
        {
            static std::optional<GameState> state;
            return state;
        }
    }

    void RegisterGameState(GameState state)
    {
        Registered() = std::move(state);
    }

    const GameState* GetGameState()
    {
        const std::optional<GameState>& state = Registered();
        return state ? &*state : nullptr;
    }
}
