#include "TrainerSettingsService.h"

TrainerSettingsService::TrainerSettingsService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit) : _httpEndpoint(
                                                                                                           TrainerSettings::read,
                                                                                                           TrainerSettings::update,
                                                                                                           this,
                                                                                                           server,
                                                                                                           TRAINER_SETTINGS_ENDPOINT_PATH,
                                                                                                           sveltekit->getSecurityManager(),
                                                                                                           AuthenticationPredicates::IS_AUTHENTICATED),
                                                                                                       _fsPersistence(
                                                                                                           TrainerSettings::read,
                                                                                                           TrainerSettings::update,
                                                                                                           this,
                                                                                                           sveltekit->getFS(),
                                                                                                           TRAINER_SETTINGS_FILE_PATH)
{
}

void TrainerSettingsService::begin()
{
    /* Load the configuration from LittleFS. When the file does not exist the
     * compiled-in defaults stay in place. */
    _fsPersistence.readFromFS();
    _httpEndpoint.begin();
}
