#include "../include/init_disc.h"
#include "../include/logs.h"

void* correr_tripulante;

void cerrar_programa(t_log* main_log, t_log* main_log_inv, t_config_disc* cfg) {
    log_destroy(main_log);
    log_destroy(main_log_inv);

    free(cfg->IP_MI_RAM_HQ);
    free(cfg->IP_I_MONGO_STORE);
    free(cfg->ALGORITMO);
    free(cfg);

    queue_destroy(COLA_TRIPULANTES);
    queue_destroy(COLA_NEW);
    queue_destroy(COLA_BLOQUEADOS);
    list_destroy(LISTA_HILOS);
    //list_destroy(COLA_EXIT);
    list_destroy(LISTA_SABOTAJE);

    rl_clear_history();
}

uint8_t generar_conexiones(int* i_mongo_store_fd, int* mi_ram_hq_fd, t_config_disc* cfg) {
    char* port_i_mongo_store = string_itoa(cfg->PUERTO_I_MONGO_STORE);
    char* port_mi_ram_hq = string_itoa(cfg->PUERTO_MI_RAM_HQ);

    *i_mongo_store_fd = crear_conexion(
            main_log,
            "I_MONGO_STORE",
            cfg->IP_I_MONGO_STORE,
            port_i_mongo_store
    );

    *mi_ram_hq_fd = crear_conexion(
            main_log,
            "MI_RAM_HQ",
            cfg->IP_MI_RAM_HQ,
            port_mi_ram_hq
    );

    free(port_i_mongo_store);
    free(port_mi_ram_hq);

    return *i_mongo_store_fd != 0 && *mi_ram_hq_fd != 0;
}

uint8_t cargar_configuracion(t_config_disc* config) {
    t_config* cfg = config_create("discordiador.config");

    if(cfg == NULL) {
        log_error(main_log, "No se encontro discordiador.config");
        return 0;
    }

    char* properties[] = {
        "IP_MI_RAM_HQ",
        "IP_I_MONGO_STORE",
        "PUERTO_I_MONGO_STORE",
        "IP_MI_RAM_HQ",
        "GRADO_MULTITAREA",
        "ALGORITMO",
        "QUANTUM",
        "DURACION_SABOTAJE",
        "RETARDO_CICLO_CPU",
        NULL
    };

    // Falta alguna propiedad
    if(!config_has_all_properties(cfg, properties)) {
        log_error(main_log, "Propiedades faltantes en el archivo de configuracion");
        config_destroy(cfg);
        return 0;
    }

    config->IP_MI_RAM_HQ = strdup(config_get_string_value(cfg, "IP_MI_RAM_HQ"));
    config->PUERTO_MI_RAM_HQ = config_get_int_value(cfg, "PUERTO_MI_RAM_HQ");
    config->IP_I_MONGO_STORE = strdup(config_get_string_value(cfg, "IP_I_MONGO_STORE"));
    config->PUERTO_I_MONGO_STORE = config_get_int_value(cfg, "PUERTO_I_MONGO_STORE");
    config->GRADO_MULTITAREA = config_get_int_value(cfg, "GRADO_MULTITAREA");
    config->ALGORITMO = strdup(config_get_string_value(cfg, "ALGORITMO"));
    config->QUANTUM = config_get_int_value(cfg, "QUANTUM");
    config->DURACION_SABOTAJE = config_get_int_value(cfg, "DURACION_SABOTAJE");
    config->RETARDO_CICLO_CPU = config_get_int_value(cfg, "RETARDO_CICLO_CPU");

    if(strncmp(config->ALGORITMO, "RR", 2) == 0)
        correr_tripulante = correr_tripulante_RR;
    else
        correr_tripulante = correr_tripulante_FIFO;

    log_info(main_log, "Archivo de configuracion cargado correctamente");

    config_destroy(cfg);

    return 1;
}