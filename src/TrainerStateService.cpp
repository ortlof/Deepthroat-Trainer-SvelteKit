#include "TrainerStateService.h"

TrainerStateService::TrainerStateService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit) : _httpEndpoint(
                                                                                                   TrainerState::read,
                                                                                                   TrainerState::update,
                                                                                                   this,
                                                                                                   server,
                                                                                                   TRAINER_STATE_ENDPOINT_PATH,
                                                                                                   sveltekit->getSecurityManager(),
                                                                                                   AuthenticationPredicates::IS_AUTHENTICATED),
                                                                                               _eventEndpoint(
                                                                                                   TrainerState::read,
                                                                                                   TrainerState::update,
                                                                                                   this,
                                                                                                   sveltekit->getSocket(),
                                                                                                   TRAINER_STATE_EVENT)
{
    /* Live data only: no FSPersistence here. The trainer task registers its own
     * update handler, which applies the commands to the running session. */
}

void TrainerStateService::begin()
{
    _httpEndpoint.begin();
    _eventEndpoint.begin();
}
