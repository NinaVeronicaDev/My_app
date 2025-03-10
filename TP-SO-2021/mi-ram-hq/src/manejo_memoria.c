#include "../include/manejo_memoria.h"
#define MIN(A,B) ((A)<(B)?(A):(B))
#define ACEITUNA (. )~

extern t_log* logger;
extern t_config_mrhq* cfg;
extern t_list* segmentos_libres;
extern void* memoria_principal;
extern uint32_t memoria_disponible;
extern segmento_t* (*proximo_hueco)(uint32_t);

extern t_list* tp_patotas;
extern frame_t* tabla_frames;
extern frame_swap_t* tabla_frames_swap;
extern void* area_swap;
extern uint32_t espacio_disponible_swap;

extern pthread_mutex_t MUTEX_MP;
extern pthread_mutex_t MUTEX_MP_BUSY;
extern sem_t SEM_COMPACTACION_DONE;
extern sem_t SEM_COMPACTACION_START;

#define INICIO_INVALIDO (cfg->TAMANIO_MEMORIA+69)

extern uint64_t global_TUR;

// Varios

static uint32_t cant_paginas(uint32_t size, size_t* rem) {
    uint32_t t_pag = cfg->TAMANIO_PAGINA;
    *rem = size % t_pag;
    return (*rem) ? size/t_pag + 1 : size/t_pag;
}

bool entra_en_mp(uint32_t tamanio) {
    size_t rem;
    return cfg->SEG
        ? memoria_disponible   >= tamanio
        : cant_frames_libres() >= cant_paginas(tamanio, &rem);
}

bool entra_en_mp_con_swap(uint32_t tamanio) {
    return tamanio <= (memoria_disponible + espacio_disponible_swap);
}

uint32_t paginas_que_entran_en_swap() {
    return espacio_disponible_swap / cfg->TAMANIO_PAGINA;
}

void dump_mp() {
    char* timestamp = temporal_get_string_time("%d_%m_%y--%H_%M_%S");
    char* filename = string_from_format("Dump_%s.dmp", timestamp);
    FILE* dump_file = fopen(filename, "w+");

    char* hr = string_repeat('-', 50);
    char* data = NULL;
    if (cfg->SEG) {
        char* str_segus = stringify_segus();
        data = string_from_format(
            "\n%s\nDump: %s\n%s\n%s\n\n",
            hr, timestamp, str_segus, hr
        );
        free(str_segus);
    }
    else {
        char* str_frames = stringify_tabla_frames();
        data = string_from_format(
            "\n%s\n\n%s\nDump: %s\n%s\n%s\n\n",
            "LOS MARCOS NO LISTADOS ESTAN LIBRES",
            hr, timestamp, str_frames, hr
        );
        free(str_frames);
    }

    fprintf(dump_file, "%s", data);

    goto die;
    die:
        fclose(dump_file);
        free(hr);
        free(data);
        free(timestamp);
        free(filename);
}

////// MANEJO MEMORIA PRINCIPAL - SEGMENTACION

bool get_structures_from_tabla_tripulante(ts_tripulante_t* tabla, TCB_t** p_tcb, PCB_t** p_pcb) {
    // Leemos TCB
    void* s_tcb = get_segmento_data(
        tabla->tcb->inicio,
        tabla->tcb->tamanio
    );
    TCB_t* tcb = deserializar_tcb(s_tcb);
    *p_tcb = tcb;

    // Leemos PCB
    void* s_pcb = get_segmento_data(tcb->dl_pcb, 8);
    PCB_t* pcb = deserializar_pcb(s_pcb);
    *p_pcb = pcb;

    free(s_tcb); free(s_pcb);
    return true;
}

bool get_structures_from_tid_segmentacion
(uint32_t tid, ts_tripulante_t** p_tabla_tripulante, TCB_t** p_tcb, PCB_t** p_pcb) {
    ts_tripulante_t* tabla_tripulante = list_find_by_tid_tstripulantes(tid);
    if (tabla_tripulante == NULL) return false;
    *p_tabla_tripulante = tabla_tripulante;

    // Leemos TCB
    void* s_tcb = get_segmento_data(
        tabla_tripulante->tcb->inicio,
        tabla_tripulante->tcb->tamanio
    );
    TCB_t* tcb = deserializar_tcb(s_tcb);
    *p_tcb = tcb;

    // Leemos PCB
    void* s_pcb = get_segmento_data(tcb->dl_pcb, 8);
    PCB_t* pcb = deserializar_pcb(s_pcb);
    *p_pcb = pcb;

    free(s_tcb); free(s_pcb);

    return true;
}

// Meter chorizo de bytes en MP y actualiza listas de registro.
// Si se llama a esta func es porque ya se sabe que entra
// Devuelve la Dir fisica del segmento, o INICIO_INVALIDO si explota (no deberia pasar)
uint32_t meter_segmento_en_mp(void* data, uint32_t size, tipo_segmento_t tipo) {
    log_info(logger, "Metiendo segmento tipo %d (size %" PRIu32 ") en memoria", tipo, size);

    segmento_t* hueco_victima = (*proximo_hueco)(size);
    if (hueco_victima == NULL) {
        if (!compactar_mp()) {
            log_error(logger, "Fallo la compactacion. QEPD.");
            return INICIO_INVALIDO;
        }

        log_info(logger, "Compacte todos!");

        hueco_victima = (*proximo_hueco)(size);
        if (hueco_victima == NULL)
            return INICIO_INVALIDO; // no hay hueco (no deberia pasar)
    }

    uint32_t inicio = hueco_victima->inicio;
    memcpy_segmento_en_mp(hueco_victima->inicio, data, size);

    if (!meter_segmento_actualizar_hueco(hueco_victima, size)) {
        log_error(logger, "Error catastrofico metiendo segmento en MP");
        return INICIO_INVALIDO;
    }

    memoria_disponible -= size;
    if (memoria_disponible < 0) {
        // Algo dentro de mi me dijo: pone esto por las dudas
        log_error(logger, "ROMPISTE TODO, QUE HICISTE?");
        return INICIO_INVALIDO;
    }

    list_add_segus(new_segmento(tipo, 0, inicio, size)); // Al final, por si exploto todo antes

    return inicio;
}

bool eliminar_segmento_de_mp(uint32_t inicio) {
    log_info(logger, "Eliminando segmento con inicio en %" PRIu32 " de memoria", inicio);

    segmento_t* segmento = list_find_by_inicio_segus(inicio);
    if (segmento == NULL) return false;

    // Nuevo hueco
    segmento_t* hueco_generado = new_segmento(0, 0, segmento->inicio, segmento->tamanio);
    // ACA ESTABA EL MERGE CON HUECO ANTERIOR Y POSTERIOR, SE FUE
    list_add_seglib(hueco_generado);

    // Importante
    memoria_disponible += segmento->tamanio;

    // Memset fisico
    memset_0_segmento_en_mp(segmento->inicio, segmento->tamanio);
    list_remove_by_inicio_segus(segmento->inicio);
    return true;
}

static void fix_inicio_seg_en_mp(segmento_t* seg, uint32_t inicio_destino) {
    // sangre sudor y lagrimas
    switch (seg->tipo) {
        case TCB_SEG:
        {
            ts_tripulante_t* tabla_trip = list_find_by_inicio_tcb_tstripulantes(seg->inicio);
            tabla_trip->tcb->inicio = inicio_destino;
            break;
        }
        case PCB_SEG:
        {
            // "Nosotros no buscamos eficiencia en el TP"
            ts_patota_t* tabla_patota = list_find_by_inicio_pcb_tspatotas(seg->inicio);
            tabla_patota->pcb->inicio = inicio_destino;

            t_list* tcbs = list_get_tcb_segments_segus();
            t_list_iterator* i_tcbs = list_iterator_create(tcbs);
            while (list_iterator_has_next(i_tcbs)) {
                segmento_t* seg_tcb = list_iterator_next(i_tcbs);
                void* s_tcb = get_segmento_data(seg_tcb->inicio, seg_tcb->tamanio);
                TCB_t* tcb = deserializar_tcb(s_tcb);
                free(s_tcb);

                if (tcb->dl_pcb == seg->inicio) {
                    tcb->dl_pcb = inicio_destino;
                    s_tcb = serializar_tcb(tcb);
                    memcpy_segmento_en_mp(seg_tcb->inicio, s_tcb, seg_tcb->tamanio);
                    free(s_tcb);
                }
                free(tcb);
            }
            list_iterator_destroy(i_tcbs);
            list_destroy(tcbs);
            break;
        }
        case TAREAS_SEG:
        {
            ts_patota_t* tabla_patota = list_find_by_inicio_tareas_tspatotas(seg->inicio);
            tabla_patota->tareas->inicio = inicio_destino;

            t_list* pcbs = list_get_pcb_segments_segus();
            t_list_iterator* i_pcbs = list_iterator_create(pcbs);
            while (list_iterator_has_next(i_pcbs)) {
                segmento_t* seg_pcb = list_iterator_next(i_pcbs);
                void* s_pcb = get_segmento_data(seg_pcb->inicio, seg_pcb->tamanio);
                PCB_t* pcb = deserializar_pcb(s_pcb);
                free(s_pcb);

                if (pcb->dl_tareas == seg->inicio) {
                    pcb->dl_tareas = inicio_destino;
                    s_pcb = serializar_pcb(pcb);
                    memcpy_segmento_en_mp(seg_pcb->inicio, s_pcb, seg_pcb->tamanio);
                    free(s_pcb);
                }
                free(pcb);
            }
            list_iterator_destroy(i_pcbs);
            list_destroy(pcbs);
            break;
        }
        default: break; // para tests
    }
}
static bool compactar_mp_iteracion(uint32_t i) {
    segmento_t* segmento = list_get_segus(i);
    if (segmento == NULL) return false;     // no existe!!
    if (segmento->inicio == 0) return true; // es el primero y esta pegadito al techo

    segmento_t* hueco = list_find_first_by_inicio_seglib(segmento->inicio-1);
    if (hueco == NULL) return true;         // ya esta compactado

    uint32_t inicio_original = segmento->inicio;
    uint32_t inicio_destino = hueco->inicio;

    // Guardamos varias DLs en memoria... hay que actualizarlas
    fix_inicio_seg_en_mp(segmento, inicio_destino);

    // Muevo segmento y muevo hueco
    segmento->inicio = hueco->inicio;
    list_sort_segus();

    hueco->inicio = segmento->inicio + segmento->tamanio;

    // Posible merge
    segmento_t* hueco_posterior = list_find_first_by_inicio_seglib(hueco->inicio+hueco->tamanio);
    if (hueco_posterior) {
        hueco->tamanio += hueco_posterior->tamanio;
        hueco_posterior->tamanio = 0;
        remove_zero_sized_gap_seglib();
    }

    // Movemos el segmento en la memoria fisica
    realloc_segmento_en_mp(inicio_original, inicio_destino, segmento->tamanio);
    return true;
}
bool compactar_mp() {
    if (cfg->SEG) {
        unificar_huecos_seglib(); // merge de huecos contiguos                                            (pasar de esquema meme a esquema sensato)
        
        if (list_is_empty_segus()) {
            return true;
        }

        log_info(logger, "Compactando memoria...");
        uint32_t segmentos = list_size_segus();
        for (int i=0; i<segmentos; i++) {
            // log_warning(logger, "Compactando segmento [%d] de %d...", i, segmentos);
            if(!compactar_mp_iteracion(i)) {
                return false;
            }
        }
        return true;
    }
    return true;
}

////// UTILS SEGMENTOS_LIBRES

static void* hueco_menor(void* h1, void* h2) {
    segmento_t* hueco1 = (segmento_t*) h1;
    segmento_t* hueco2 = (segmento_t*) h2;
    return hueco1->tamanio < hueco2->tamanio? h1 : h2;
}

// Meto un segmento nuevo a la MP, tengo menos huecos
bool meter_segmento_actualizar_hueco(segmento_t* hueco_target, uint32_t tamanio) {
    // Cosas que no deberian pasar nunca:
    if (hueco_target == NULL || (hueco_target->tamanio < tamanio)) //??
        return false;

    if (hueco_target->tamanio == tamanio) {
        hueco_target->tamanio = 0;
        remove_zero_sized_gap_seglib(); // no es lo mas eficiente/elegante, pero si lo mas facil
        return true;
    }

    hueco_target->inicio += tamanio;
    hueco_target->tamanio -= tamanio;
    return true;
}

// De todos los huecos, elige el que minimiza el espacio desaprovechado (si hay empate, el ultimo)
segmento_t* proximo_hueco_best_fit(uint32_t tamanio) {
    t_list* huecos_disponibles = list_filter_by_min_size_seglib(tamanio);

    if (list_size(huecos_disponibles) == 0) {
        list_destroy(huecos_disponibles);
        return NULL;
    }
    segmento_t* ret = (segmento_t*) list_get_minimum(huecos_disponibles, (void*) &hueco_menor);
    list_destroy(huecos_disponibles);

    return ret;
}

// De todos los huecos, elige el primero en el que entra el tamanio dado
segmento_t* proximo_hueco_first_fit(uint32_t tamanio) {
    return list_find_first_by_min_size_seglib(tamanio);
}

void compactar_segmentos_libres() {
    list_clean_seglib();
    list_add_seglib(
        new_segmento(
            0, 0,
            cfg->TAMANIO_MEMORIA-memoria_disponible,
            memoria_disponible
        )
    );
}


////// MANEJO MEMORIA PRINCIPAL - PAGINACION


// ESTA FUNCION PODRIA TENER CONDICIONES DE CARRERA CON SWAP
// POCO PROBABLE, PERO POR LAS DUDAS USAR CON MUTEX_MP_BUSY
void* RACE_read_from_mp_pid_pagina_offset_tamanio
(uint32_t pid, uint32_t pagina, uint32_t offset, uint32_t tamanio) {
    void* data = malloc((size_t) tamanio);
    memset(data, 0, tamanio); //porlas

    // Quiero la tabla de paginas
    tp_patota_t* tabla_patota = list_find_by_pid_tppatotas(pid);
    t_list* paginas = tabla_patota->paginas;

    // Leo de las paginas que precise... Considerando que la imagen del proceso esta cargada de forma contigua
    uint32_t tamanio_restante = tamanio;
    for (int p=0; tamanio_restante>0; p++) {
        // inner function
        /*log_info(logger, "Leyendo pagina %" PRIu32 "+%d (>>%" PRIu32 ") de proceso %" PRIu32,
            pagina, p, offset, pid
        );*/
        //print_tppatotas(true);
        bool has_page_number(void* x) {
            entrada_tp_t* elem = (entrada_tp_t*) x;
            return elem->nro_pagina == pagina+p;
        }
        entrada_tp_t* pag = list_find(paginas, &has_page_number); // cond. de carrera aca si una pag. cambia de frame
        
        // Chequeo y actualizacion de bits
        if (pag->bit_P == 0) {
            // SWAP BUSINESS
            //log_info(logger, "No esta en RAM... la traigo de SWAP");
            if (!traer_pagina_de_swap(pid, pagina+p)) {
                log_error(logger, "Error terrible trayendo pagina de SWAP a MP. Voy a segfaultearme todo.");
                free(data);
                return NULL; // aca va tu segfault lince
            }
            //log_info(logger, "Traida!");
        }
        if (cfg->LRU) pag->TUR   = global_TUR++;
        else          pag->bit_U = 1;

        /*log_info(logger,
            "Si falla el get_pagina_data (frame %" PRIu32 ") este es el ultimo mensaje...",
            pag->nro_frame
        );*/
        void* buf = get_pagina_data(pag->nro_frame);
        //log_info(logger, "Bueno, no fallo...!");

        uint32_t bytes_a_leer = p==0
            ? MIN(cfg->TAMANIO_PAGINA - offset, tamanio) // primera pag
            : MIN(cfg->TAMANIO_PAGINA, tamanio_restante) // medio o ultima
        ;

        // !!! THIS SHIT DOESN'T WORK!! WHY? HAS I EVER?
        /*log_info(logger, "Misc: tamanio/t_restante/p/offset/bytes_a_leer"
            " - %" PRIu32 "/%" PRIu32 "/%d/%" PRIu32 "/%" PRIu32,
            tamanio, tamanio_restante, p, offset, bytes_a_leer
        );*/
        memcpy(
            data + (tamanio - tamanio_restante),
            buf + (p==0? offset : 0),
            bytes_a_leer
        );

        //log_info(logger, "Iteracion %d, leidos %" PRIu32 " de %" PRIu32, p, bytes_a_leer, tamanio);
        /*for (int i = 0; i < tamanio; i++) {
            log_info(logger, "%02x", ((unsigned char*) data) [i]);
        }*/

        if (tamanio_restante < bytes_a_leer) tamanio_restante = 0;
        else tamanio_restante -= bytes_a_leer;
        free(buf);
    }

    return data;
}

// ESTA FUNCION PODRIA TENER CONDICIONES DE CARRERA CON SWAP (ver RACE_read_from_mp_pid_pagina_offset_tamanio)
// POCO PROBABLE, PERO POR LAS DUDAS USAR CON MUTEX_MP_BUSY
bool RACE_get_structures_from_tid_paginacion
(uint32_t tid, tid_pid_lookup_t** p_tabla, TCB_t** p_tcb, PCB_t** p_pcb) {
    tid_pid_lookup_t* tabla = list_tid_pid_lookup_find_by_tid(tid);
    if (tabla==NULL) return false;
    *p_tabla = tabla;

    void* s_tcb = RACE_read_from_mp_pid_pagina_offset_tamanio(
        tabla->pid, tabla->nro_pagina, tabla->inicio, 21
    );

    /*for (int i = 0; i < 21; i++) {
        log_info(logger, "%02x", ((unsigned char*) s_tcb) [i]);
    }*/

    TCB_t* tcb = deserializar_tcb(s_tcb);
    log_info(logger, "TID#%" PRIu32 " TCB->DL_PCB: 0x%08" PRIx32, tid, tcb->dl_pcb);
    *p_tcb = tcb;
    free(s_tcb);

    void* s_pcb = RACE_read_from_mp_pid_pagina_offset_tamanio(
        tabla->pid, (tcb->dl_pcb)>>16, tcb->dl_pcb&0x00FF, 8
    );
    PCB_t* pcb = deserializar_pcb(s_pcb);
    *p_pcb = pcb;
    free(s_pcb);

    return true;
}

// La funcion que de verdad mete data en una pagina y actualiza estructuras admin. y MP
// Si se llama, es porque hay espacio en MP o en SWAP
static bool meter_pagina_en_mp(void* data, size_t size, uint32_t pid, uint32_t iter, bool offset) {
    // iter es la pagina numero i que esta metiendo en esta vuelta
    // offset indica si se empezo a cargar en una pag. que estaba por la mitad

    uint32_t inicio;
    uint32_t frame_libre = primer_frame_libre_framo(pid, &inicio);
    //log_info(logger, "Iter (%" PRIu32 "), hubo offset? (%d)", iter, offset);
    //log_info(logger, "Primer frame libre: %" PRIu32 ", inicio %" PRIu32, frame_libre, inicio);
    
    bool en_mp = true;
    if (frame_libre == 0xFFFF) {
        en_mp = false;
        const uint32_t paginas_swap = cfg->TAMANIO_SWAP/cfg->TAMANIO_PAGINA;
        //log_info(logger, "meter_pagina_en_mp: la cargo en swap eh");
        // LA CARGA EN SWAP DIRECTAMENTE

        // Busco donde joracas meterla en swap, pagina por el medio o nueva?
        uint32_t frame_libre_swap = 0;
        for (; frame_libre_swap < paginas_swap; frame_libre_swap++) {
            /*log_info(logger, "meter_pagina_en_mp: mirando... PID %" PRIu32 " | INICIO %" PRIu32,
                tabla_frames_swap[frame_libre_swap].pid, tabla_frames_swap[frame_libre_swap].inicio
            );*/
            if (
                tabla_frames_swap[frame_libre_swap].pid==pid &&
                tabla_frames_swap[frame_libre_swap].inicio!=0 &&
                tabla_frames_swap[frame_libre_swap].inicio<cfg->TAMANIO_PAGINA
            ) {
                break;
            }
        }

        if (frame_libre_swap == paginas_swap) {
            // Nueva...
            //log_info(logger, "meter_pagina_en_mp: es una nueva en swap!");
            frame_libre_swap = 0;
            while (
                frame_libre_swap < paginas_swap &&
                tabla_frames_swap[frame_libre_swap].pid!=0
            ) frame_libre_swap++;
        }
        //log_info(logger, "meter_pagina_en_mp: va en el %" PRIu32 " de swap", frame_libre_swap);

        frame_swap_t frame_swap = tabla_frames_swap[frame_libre_swap];
        memcpy(
            area_swap+frame_libre_swap*cfg->TAMANIO_PAGINA + frame_swap.inicio,
            data,
            size
        );

        frame_libre = frame_libre_swap;
        espacio_disponible_swap += cfg->TAMANIO_PAGINA;
    }

    uint32_t nro_frame = frame_libre; // ben

    //log_info(logger, "meter_pagina_en_mp: creo que paso todo. Vamos al final");

    if (en_mp) ocupar_frame_framo(nro_frame, size, pid);                  // admin.
    if (en_mp) memcpy_pagina_en_frame_mp(nro_frame, inicio, data, size);  // MP
    list_add_page_frame_tppatotas(pid, nro_frame, size, en_mp);           // admin.

    //print_swap(true);

    /*log_info(logger,
        "Ocupe el frame %" PRIu32 " desde el inicio %" PRIu32 " con data de size %zu\n",
        nro_frame, inicio, size
    );*/
    return true;
}

// Dado un stream de bytes, lo mete en MP donde encuentre paginas libres
// O si la ultima del proceso esta por la mitad, empieza por ahi
// Esta funcion nunca deberia llamarse si no hay espacio para meter la data (ni en SWAP)
uint32_t append_data_to_patota_en_mp(void* data, size_t size, uint32_t pid, bool* nuevapag) {
    log_info(logger, "Agregando data (size %zu) a patota PID#%" PRIu32 " en memoria", size, pid);
    void* buf;
    uint32_t t_pag = cfg->TAMANIO_PAGINA;

    // Data de la primera pag libre, para saber si esta por la mitad o que
    //log_info(logger, "Buscando frame libre... (size %zu)", size);
    //print_framo(true);     // debug
    //print_tppatotas(true); // debug

    uint32_t offset = 0;
    uint32_t frame_de_pag_fragmentada = primer_frame_libre_framo(pid, &offset);
    //log_info(logger, "Encontre el %" PRIu32, frame_de_pag_fragmentada);
    if (frame_de_pag_fragmentada == 0xFFFF) {
        // LA CARGA DIRECTAMENTE EN SWAP, se encarga la static bool meter_pagina_en_mp
        // Puede ser una pagina nueva o una que esta a medias
        int64_t ultima_pag_pid = -1;
        for (int i=0; i<cfg->TAMANIO_SWAP/cfg->TAMANIO_PAGINA; i++) {
            if (
                tabla_frames_swap[i].pid==pid &&
                tabla_frames_swap[i].nro_pagina > ultima_pag_pid &&
                tabla_frames_swap[i].inicio < cfg->TAMANIO_PAGINA
            ) {
                ultima_pag_pid = tabla_frames_swap[i].nro_pagina;
            }
        }
        //ultima_pag_pid==-1? log_info(logger, "Pagina nueva en SWAP") : log_info(logger, "Pagina existente en SWAP");
        for (int i=0; i<cfg->TAMANIO_SWAP/cfg->TAMANIO_PAGINA; i++) {
            if (ultima_pag_pid == -1) {
                offset = 0;
                break;
            }
            if (tabla_frames_swap[i].pid==pid && tabla_frames_swap[i].nro_pagina==ultima_pag_pid) {
                offset = tabla_frames_swap[i].inicio;
                /*log_info(logger, "Va a swap a donde PID/NRO/INICIO = %d/%d/%d",
                    tabla_frames_swap[i].pid, tabla_frames_swap[i].nro_pagina, tabla_frames_swap[i].inicio
                );*/
                break;
            }
        }
    }
    *nuevapag = offset==0; // SIGNIFICA QUE INAUGURA UNA NUEVA PAGINA

    size_t rem = 0;
    size_t size_ajustado; // sin el "cachito que sobra porque entra en la pag fragmentada"
    if (offset) {
        if (t_pag - offset > size)  size_ajustado = 0;
        else                        size_ajustado = size - (t_pag - offset);
    }
    else                            size_ajustado = size;

    uint32_t n_pags = cant_paginas(size_ajustado, &rem);  // iteraciones sin offset (sin el "cachito")
    if (offset) n_pags++;                                 // iteraciones ajustadas  (el "cachito")

    //log_info(logger, "\nVou inserir um size %zu com rem %zu e offset %" PRIu32 " fazendo %" PRIu32 " iteracoes",
    //   size, rem, offset, n_pags);

    // Itera de a una pagina y las mete en MP
    uint32_t n_iteraciones = n_pags;                    // +0.5 profes de PDP

    for (uint32_t i=0; i<n_iteraciones; i++) {
        size_t size_chunk = i==0
            ? MIN(t_pag - offset, size)                 // Primera pagina, posible offset
            : rem && (i==n_iteraciones-1)? rem : t_pag; // Otras paginas, si es la ultima es tamanio rem si hay rem

        //log_info(logger, "Chunk inserido: %zu", size_chunk);
        buf = malloc(size_chunk);
        if (i == 0)      memcpy(buf, data,                          size_chunk); // primera
        else if (offset) memcpy(buf, data+t_pag-offset+(i-1)*t_pag, size_chunk); // completas intermedias o final (hubo offset)
        else             memcpy(buf, data+i*t_pag,                  size_chunk); // completas intermedias o final (no hubo offset)

        //log_info(logger, "Antes de meter_pagina_en_mp");
        if (!meter_pagina_en_mp(buf, size_chunk, pid, i, !!offset)) {
            free(buf);
            return 0xFFFF;
        }
        //log_info(logger, "Despues de meter_pagina_en_mp");
        free(buf);
    }

    return offset;
}

// ESTA FUNCION PODRIA TENER CONDICIONES DE CARRERA CON SWAP
// POCO PROBABLE, PERO POR LAS DUDAS USAR CON MUTEX_MP_BUSY
// Esta va para vos, Terry
bool RACE_actualizar_tcb_en_mp(uint32_t pid, TCB_t* tcb) {
    log_info(logger, "Actualizando TCB de TID#%" PRIu32 " en memoria", tcb->tid);
    const size_t size_tcb = 21;
    void* s_tcb = serializar_tcb(tcb);

    // Recupero entrada_tp_t de la primera pagina del TCB
    tid_pid_lookup_t* lookup = list_tid_pid_lookup_find_by_tid(tcb->tid);
    if (lookup==NULL) {
        log_error(logger, "Error (1) actualizando TCB de TID#%" PRIu32, tcb->tid);
        free(s_tcb);
        return false;
    }
    tp_patota_t* tabla_patota = list_find_by_pid_tppatotas(pid);
    if (tabla_patota==NULL) {
        log_error(logger, "Error (2) actualizando TCB de TID#%" PRIu32, tcb->tid);
        free(s_tcb);
        return false;
    }
    t_list* paginas = tabla_patota->paginas;

    bool has_page_number(void* x) {
        entrada_tp_t* elem = (entrada_tp_t*) x;
        return elem->nro_pagina == lookup->nro_pagina;
    }

    retry:; // hacker
    entrada_tp_t* pagina = list_find(paginas, &has_page_number); // posible condicion de RAZA
    entrada_tp_t* pagina_2 = NULL;

    if (pagina->bit_P) {
        // Presente en RAM
        if (lookup->inicio+size_tcb > cfg->TAMANIO_PAGINA) {
            // 2 paginas
            size_t primer_size = cfg->TAMANIO_PAGINA - lookup->inicio;
            size_t segundo_size = size_tcb - primer_size;

            bool has_page_number(void* x) {
                entrada_tp_t* elem = x;
                return elem->nro_pagina == lookup->nro_pagina + 1;
            }
            bed:;
            pagina_2 = list_find(paginas, &has_page_number); // posible condicion de RAZA
            if (pagina_2->bit_P==0) goto bringme;

            memcpy_segmento_en_mp(
                pagina->nro_frame*cfg->TAMANIO_PAGINA + lookup->inicio, s_tcb, primer_size
            );
            memcpy_segmento_en_mp(
                pagina_2->nro_frame*cfg->TAMANIO_PAGINA, s_tcb+primer_size, segundo_size
            );

        }
        else {
            // 1 pagina
            memcpy_segmento_en_mp(
                pagina->nro_frame*cfg->TAMANIO_PAGINA + lookup->inicio, s_tcb, size_tcb
            ); // ya se que no es un segmento, pero me sirve la funcion
        }
    }
    else {
        // SWAP
        traer_pagina_de_swap(pid, pagina->nro_pagina);

        goto retry;
    }
    free(s_tcb);
    return true;

    bringme:; // https://www.youtube.com/watch?v=3YxaaGgTQYM
    if (pagina_2) traer_pagina_de_swap(pid, pagina_2->nro_pagina);
    goto bed;
}

bool delete_patota_en_mp(uint32_t pid) {
    log_info(logger, "Eliminando patota PID#%" PRIu32 " de memoria", pid);

    tp_patota_t* tabla_patota = list_remove_by_pid_tppatotas(pid);
    if (tabla_patota == NULL) return false;

    t_list* paginas = tabla_patota->paginas;
    t_list_iterator* i_paginas = list_iterator_create(paginas);
    while (list_iterator_has_next(i_paginas)) {
        entrada_tp_t* pagina = list_iterator_next(i_paginas);
        /*log_info(logger, "Borrando pagina %" PRIu32 " de PID %" PRIu32 " en frame %" PRIu32 " (%d)...",
            pagina->nro_pagina, pid, pagina->nro_frame, pagina->bit_P
        );*/
        if (pagina->bit_P) {
            // Esta en MP
            clear_frame_en_mp(pagina->nro_frame);
            liberar_frame_framo(pagina->nro_frame);
        }
        else {
            // Esta en SWAP
            memset(area_swap+pagina->nro_frame*cfg->TAMANIO_PAGINA, 0, cfg->TAMANIO_PAGINA);
            tabla_frames_swap[pagina->nro_frame].pid = 0;
            tabla_frames_swap[pagina->nro_frame].nro_pagina = 0xFFFF;
            tabla_frames_swap[pagina->nro_frame].inicio = 0;
        }
    }
    list_iterator_destroy(i_paginas);

    free_tp_patota_t((void*) tabla_patota);
    return true;
}

// SWAP

static uint32_t pagina_a_reemplazar_LRU(uint32_t frame_a_swap, uint32_t* pid, uint32_t* nro_pagina) {
    uint32_t TUR_minimo = 0xFFFF;
    entrada_tp_t* entrada_victima = NULL;

    t_list_iterator* i_tp_patotas = list_iterator_create(tp_patotas);
    while (list_iterator_has_next(i_tp_patotas)) {
        tp_patota_t* tabla_patota = list_iterator_next(i_tp_patotas);

        t_list_iterator* i_paginas = list_iterator_create(tabla_patota->paginas);
        while (list_iterator_has_next(i_paginas)) {
            entrada_tp_t* pagina = list_iterator_next(i_paginas);

            if (pagina->bit_P && pagina->TUR<TUR_minimo) {
                TUR_minimo = pagina->TUR;
                entrada_victima = pagina;
                *pid = tabla_patota->pid;
                *nro_pagina = pagina->nro_pagina;
            }
        }
        list_iterator_destroy(i_paginas);
    }
    list_iterator_destroy(i_tp_patotas);

    entrada_victima->bit_P = 0;
    uint32_t frame = entrada_victima->nro_frame;
    entrada_victima->nro_frame = frame_a_swap;
    return frame;
}
#define BEN(a,b) b
static uint32_t pagina_a_reemplazar_CLOCK(uint32_t frame_a_swap, uint32_t* pid, uint32_t* nro_pagina) {
    // "No buscamos eficiencia en el TP", parte 654356
    // 1427 profes de PDP se retuercen del dolor y los ojos les sangran chocolate

    // Como tengo las tablas con bit_U en cada tabla de patota... necesito concatenar todas en una lista
    struct horrible {
        entrada_tp_t* pagina;
        uint32_t pid;
    };
    t_list* frames_presentes = list_create();

    // MIRAR BAJO LA SUPERVISION DE UN ADULTO RESPONSABLE
    t_list_iterator* i_tp_patotas = list_iterator_create(tp_patotas);
    while (list_iterator_has_next(i_tp_patotas)) {
        tp_patota_t* tabla_patota = list_iterator_next(i_tp_patotas);

        t_list_iterator* i_paginas = list_iterator_create(tabla_patota->paginas);
        while (list_iterator_has_next(i_paginas)) {
            entrada_tp_t* pagina = list_iterator_next(i_paginas);
            if (pagina->bit_P==0) continue;
            struct horrible* pagina_patota = malloc(sizeof(struct horrible));
            pagina_patota->pagina = pagina;
            pagina_patota->pid = tabla_patota->pid;
            list_add(frames_presentes, (void*) pagina_patota);
        }
        list_iterator_destroy(i_paginas);
    }
    list_iterator_destroy(i_tp_patotas);

    // yyy ordenarla por nro de frame
    bool frame_precede_frame(void* e1, void* e2) {
        struct horrible* entrada1 = e1;
        struct horrible* entrada2 = e2;
        return entrada1->pagina->nro_frame < entrada2->pagina->nro_frame;
    }
    list_sort(frames_presentes, &frame_precede_frame);

    // Ahora si puedo aplicar el algoritmo del RELOJITO
    const uint32_t cant_frames = list_size(frames_presentes);
    static uint64_t nro_frame_posible_victima = 0;
    while (BEN(@ RELOJITO @,'@')) { // si no lo obfuscaba, era demasiado buena esta funcion ya
        struct horrible* posible_victima = list_get(frames_presentes, nro_frame_posible_victima % cant_frames);
        if (posible_victima->pagina->bit_U == 0) {
            posible_victima->pagina->bit_P = 0;
            posible_victima->pagina->nro_frame = frame_a_swap;
            *pid = posible_victima->pid;
            *nro_pagina = posible_victima->pagina->nro_pagina;
            break;
        }
        posible_victima->pagina->bit_U = 0;

        nro_frame_posible_victima++;
    }

    list_destroy_and_destroy_elements(frames_presentes, (void*) free);

    uint32_t ans = nro_frame_posible_victima % (cfg->TAMANIO_MEMORIA/cfg->TAMANIO_PAGINA);
    nro_frame_posible_victima++;
    return ans;
}

uint32_t pagina_a_reemplazar(uint32_t frame_a_swap, uint32_t* pid, uint32_t* nro_pagina) {
    log_info(logger, "Buscando pagina para reemplazar...");
    return cfg->LRU
        ? pagina_a_reemplazar_LRU(frame_a_swap, pid, nro_pagina)
        : pagina_a_reemplazar_CLOCK(frame_a_swap, pid, nro_pagina);
}

// Se llama cuando bit_P == 0 y necesito leer la pagina
// Es que yo ya a esta altura no puedo pensar una solucion eficiente y con codigo limpio, lo lamento profundamente
bool traer_pagina_de_swap(uint32_t pid, uint32_t nro_pagina) {
    log_info(logger, "Trayendo pagina de SWAP a RAM");
    /*log_info(logger, "traer_pagina_de_swap: Voy a buscar cosas para PID: %" PRIu32 " N_P: %" PRIu32,
        pid, nro_pagina
    );*/

    // Mi pagina deseada esta aca:
    const uint32_t paginas_swap = cfg->TAMANIO_SWAP/cfg->TAMANIO_PAGINA;
    uint32_t frame_deseado_swap = 0;
    for (; frame_deseado_swap < paginas_swap; frame_deseado_swap++) {
        /*log_info(logger, "traer_pagina_de_swap: mirando PID: %" PRIu32 " N_P: %" PRIu32 " INICIO: %" PRIu32,
            tabla_frames_swap[frame_deseado_swap].pid,
            tabla_frames_swap[frame_deseado_swap].nro_pagina,
            tabla_frames_swap[frame_deseado_swap].inicio
        );*/
        if (
            tabla_frames_swap[frame_deseado_swap].pid == pid &&
            tabla_frames_swap[frame_deseado_swap].nro_pagina == nro_pagina
        ) break;
    }
    /*log_info(logger, "traer_pagina_de_swap: parece que es el frame %" PRIu32 " en swap...",
        frame_deseado_swap
    );*/

    uint32_t inicio; // primero lo ignoramos y despues lo reutilizamos
    uint32_t frame_libre = primer_frame_libre_framo(0xCACA, &inicio); // pid gigante que nunca vamos a tener para que ignore paginas por la mitad

    if (frame_libre == 0xFFFF) {
        //log_info(logger, "traer_pagina_de_swap: No hay frame libre en MP, hacemos cambioloco");
        // NO HAY FRAME LIBRE EN MP
        // Tengo que rajar una pagina (LRU | CLOCK). Cambio esa pagina por la deseada. UN SWAP.
        uint32_t pid_rajado, pagina_rajada, inicio_frame_victima;
        frame_libre = pagina_a_reemplazar(frame_deseado_swap, &pid_rajado, &pagina_rajada);
        //log_info(logger, "Frame a reemplazar (MP): %" PRIu32, frame_libre);

        inicio_frame_victima = tabla_frames[frame_libre].inicio_hueco;
        inicio = tabla_frames_swap[frame_deseado_swap].inicio;
        /*log_info(logger, "traer_pagina_de_swap: Resulta que reemplazamos el frame %" PRIu32 " de MP"
            " donde estaba la pagina %" PRIu32 " de PID#%" PRIu32 " con hueco iniciado en %" PRIu32,
            frame_libre, pagina_rajada, pid_rajado, inicio_frame_victima
        );*/

        // Data de las paginas
        void* data_pagina_mp = get_pagina_data(frame_libre);

        void* data_pagina_swap = malloc(cfg->TAMANIO_PAGINA);
        memset(data_pagina_swap, 0b0, cfg->TAMANIO_PAGINA);
        memcpy(
            data_pagina_swap,
            area_swap+frame_deseado_swap*cfg->TAMANIO_PAGINA,
            cfg->TAMANIO_PAGINA
        );

        // CAMBIO CAMBIO CAMBIOOOO DOLAR EURO CAMBIOOO SWAP SWAP SWAP SWAP SWAP stroke
        clear_frame_en_mp(frame_libre);
        memcpy_pagina_en_frame_mp(frame_libre, 0, data_pagina_swap, cfg->TAMANIO_PAGINA);
        memcpy(
            area_swap+frame_deseado_swap*cfg->TAMANIO_PAGINA,
            data_pagina_mp,
            cfg->TAMANIO_PAGINA
        );

        free(data_pagina_mp);
        free(data_pagina_swap);

        tabla_frames_swap[frame_deseado_swap].pid = pid_rajado;
        tabla_frames_swap[frame_deseado_swap].inicio = inicio_frame_victima;
        tabla_frames_swap[frame_deseado_swap].nro_pagina = pagina_rajada;

        //log_info(logger, "traer_pagina_de_swap: Fin de la falopa");
    }
    else {
        // HAY FRAME LIBRE EN MP

        // Leo data de SWAP
        void* data = malloc(cfg->TAMANIO_PAGINA);
        memcpy(
            data,
            area_swap+frame_deseado_swap*cfg->TAMANIO_PAGINA,
            cfg->TAMANIO_PAGINA
        );

        // Limpio frame en swap
        memset(
            area_swap+frame_deseado_swap*cfg->TAMANIO_PAGINA,
            0,
            cfg->TAMANIO_PAGINA
        );
        espacio_disponible_swap -= cfg->TAMANIO_PAGINA;
        tabla_frames_swap[frame_deseado_swap].pid = 0;

        // Carga en MP
        clear_frame_en_mp(frame_libre);
        memcpy_pagina_en_frame_mp(frame_libre, 0, data, cfg->TAMANIO_PAGINA);
        free(data);

        inicio = tabla_frames_swap[frame_deseado_swap].inicio;
    }

    // Actualizo estado de pagina en TPPATOTAS y FRAMO (IMPORTANTE!)
    //log_info(logger, "Framo: frame/inicio/pid %" PRIu32 "/%" PRIu32 "/%" PRIu32 "", frame_libre, inicio, pid);
    list_indicar_pagina_en_frame_tppatotas(pid, nro_pagina, frame_libre);
    liberar_frame_framo(frame_libre);
    ocupar_frame_framo(
        frame_libre,
        inicio,
        pid
    );

    return true;
}
