/* Mini host CLAP para SoloSampler: carga el .clap, instancia el plugin,
 * procesa audio (solo salida - es un instrumento), opcionalmente inyecta un
 * sample real via clap.state (sin necesitar drag-and-drop/click real - ver
 * memoria del proyecto sobre por que no se puede simular input en este
 * sandbox) y opcionalmente abre la GUI standalone para verificacion visual.
 * Uso: mini_host <ruta al .clap> [ruta a un .wav] [gui]
 * Adaptado del mini_host.c de NeoLooper (sibling project). */
#include <clap/clap.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const void* host_get_extension(const struct clap_host* h, const char* id) {
    (void)h; (void)id;
    return NULL;
}
static void host_request_restart(const struct clap_host* h) { (void)h; }
static void host_request_process(const struct clap_host* h) { (void)h; }
static void host_request_callback(const struct clap_host* h) { (void)h; }

typedef struct {
    const clap_event_header_t* events[4];
    uint32_t count;
} EvCtx;

static uint32_t ev_size(const struct clap_input_events* l) {
    return ((EvCtx*)l->ctx)->count;
}
static const clap_event_header_t* ev_get(const struct clap_input_events* l,
                                         uint32_t i) {
    return ((EvCtx*)l->ctx)->events[i];
}
static bool ev_push(const struct clap_output_events* l,
                    const clap_event_header_t* e) {
    (void)l; (void)e;
    return true;
}

/* stream de estado en memoria (mismo patron que NeoLooper/SchoolLooper) */
typedef struct {
    uint8_t buf[1 << 20];
    uint64_t len, pos;
} MemStream;

static int64_t mem_write(const struct clap_ostream* s, const void* d, uint64_t n) {
    MemStream* m = (MemStream*)s->ctx;
    if (m->len + n > sizeof(m->buf)) n = sizeof(m->buf) - m->len;
    memcpy(m->buf + m->len, d, n);
    m->len += n;
    return (int64_t)n;
}
static int64_t mem_read(const struct clap_istream* s, void* d, uint64_t n) {
    MemStream* m = (MemStream*)s->ctx;
    uint64_t left = m->len - m->pos;
    if (n > left) n = left;
    memcpy(d, m->buf + m->pos, n);
    m->pos += n;
    return (int64_t)n;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "uso: %s <plugin.clap> [sample.wav] [gui]\n", argv[0]);
        return 2;
    }
    const char* samplePath = NULL;
    const char* multisampleRegionsFile = NULL; /* pre-flattened regions text, "multisample=<path>" */
    const char* multisamplePathOverride = NULL; /* "multisamplepath=<fake path>", see below */
    float tuningFreqArg = 440.0f; /* "tuningfreq=<hz>", overrides the injected Pitch-tab tuning
                                    * frequency - lets a test prove sfizz_set_tuning_frequency
                                    * (RT-only, applied via SfizzEngine::renderBlock) actually
                                    * shifts pitch, via the zero-crossing count printed below. */
    const char* scalaPathArg = NULL; /* "scalapath=<file.scl>", injects a Scala tuning file path
                                       * into the same state field plugin.cpp's restoreScala()
                                       * reads (CT+OFF sfizz_load_scala_file) - same
                                       * zero-crossing-count proof as tuningfreq above. */
    int wantGui = 0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "gui")) wantGui = 1;
        else if (!strncmp(argv[i], "multisample=", 12)) multisampleRegionsFile = argv[i] + 12;
        /* Overrides the multisamplePath embedded in the injected state
         * without changing which local file multisampleRegionsFile's
         * *content* is actually read from - lets a test simulate "the
         * source .sfz has since moved/been deleted" (plugin.cpp's stateLoad
         * multisampleError check) while still reading real region text from
         * a file that does exist on disk. */
        else if (!strncmp(argv[i], "multisamplepath=", 16)) multisamplePathOverride = argv[i] + 16;
        else if (!strncmp(argv[i], "tuningfreq=", 11)) tuningFreqArg = (float)atof(argv[i] + 11);
        else if (!strncmp(argv[i], "scalapath=", 10)) scalaPathArg = argv[i] + 10;
        else samplePath = argv[i];
    }
    void* dl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    const clap_plugin_entry_t* entry =
        (const clap_plugin_entry_t*)dlsym(dl, "clap_entry");
    if (!entry) {
        fprintf(stderr, "sin clap_entry\n");
        return 1;
    }
    if (!entry->init(argv[1])) {
        fprintf(stderr, "entry->init fallo\n");
        return 1;
    }
    const clap_plugin_factory_t* factory =
        (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory || factory->get_plugin_count(factory) < 1) {
        fprintf(stderr, "factory vacia\n");
        return 1;
    }
    const clap_plugin_descriptor_t* desc =
        factory->get_plugin_descriptor(factory, 0);
    printf("plugin: %s (%s) v%s\n", desc->name, desc->id, desc->version);

    clap_host_t host = {
        .clap_version = CLAP_VERSION_INIT,
        .host_data = NULL,
        .name = "mini_host",
        .vendor = "test",
        .url = "",
        .version = "1.0",
        .get_extension = host_get_extension,
        .request_restart = host_request_restart,
        .request_process = host_request_process,
        .request_callback = host_request_callback,
    };

    const clap_plugin_t* plug = factory->create_plugin(factory, &host, desc->id);
    if (!plug || !plug->init(plug)) {
        fprintf(stderr, "no se pudo crear/init\n");
        return 1;
    }
    if (!plug->activate(plug, 48000.0, 32, 512)) {
        fprintf(stderr, "activate fallo\n");
        return 1;
    }
    if (!plug->start_processing(plug)) {
        fprintf(stderr, "start_processing fallo\n");
        return 1;
    }

    /* SoloSampler es un instrumento: sin audio input, solo salida. Procesa
     * unos bloques, incluyendo un note-on/note-off de prueba (raw MIDI, ya
     * que el note-port solo declara CLAP_NOTE_DIALECT_MIDI), y comprueba
     * que no crashea ni devuelve NaN (silencio esperado: no hay sample
     * cargado en el <region> todavia, eso es fase 4). */
    enum { N = 256 };
    float outL[N], outR[N];
    float* outChans[2] = {outL, outR};
    clap_audio_buffer_t outBuf = {.data32 = outChans, .channel_count = 2};
    EvCtx evCtx = {0};
    clap_input_events_t inEv = {.ctx = &evCtx, .size = ev_size, .get = ev_get};
    clap_output_events_t outEv = {.ctx = NULL, .try_push = ev_push};
    clap_process_t proc = {
        .steady_time = 0,
        .frames_count = N,
        .transport = NULL,
        .audio_inputs = NULL,
        .audio_outputs = &outBuf,
        .audio_inputs_count = 0,
        .audio_outputs_count = 1,
        .in_events = &inEv,
        .out_events = &outEv,
    };

    clap_event_midi_t noteOn = {
        .header = {.size = sizeof(clap_event_midi_t), .time = 0,
                   .space_id = CLAP_CORE_EVENT_SPACE_ID, .type = CLAP_EVENT_MIDI, .flags = 0},
        .port_index = 0,
        .data = {0x90, 60, 100},
    };
    clap_event_midi_t noteOff = {
        .header = {.size = sizeof(clap_event_midi_t), .time = 0,
                   .space_id = CLAP_CORE_EVENT_SPACE_ID, .type = CLAP_EVENT_MIDI, .flags = 0},
        .port_index = 0,
        .data = {0x80, 60, 0},
    };

    for (int b = 0; b < 8; ++b) {
        for (int i = 0; i < N; ++i) outL[i] = outR[i] = 1.f; /* sentinel */
        evCtx.count = 0;
        if (b == 0) evCtx.events[evCtx.count++] = &noteOn.header;
        if (b == 4) evCtx.events[evCtx.count++] = &noteOff.header;
        clap_process_status st = plug->process(plug, &proc);
        if (st == CLAP_PROCESS_ERROR) {
            fprintf(stderr, "process devolvio error\n");
            return 1;
        }
        for (int i = 0; i < N; ++i) {
            if (outL[i] != outL[i] || outR[i] != outR[i]) { /* NaN check */
                fprintf(stderr, "NaN en la salida (bloque %d, frame %d)\n", b, i);
                return 1;
            }
        }
    }
    printf("process: OK (%d bloques con note-on/off, sin crash, sin NaN)\n", 8);

    plug->stop_processing(plug);

    if (samplePath || multisampleRegionsFile) {
        const clap_plugin_state_t* state =
            (const clap_plugin_state_t*)plug->get_extension(plug, CLAP_EXT_STATE);
        if (!state) {
            fprintf(stderr, "el plugin no expone clap.state\n");
            return 1;
        }

        /* Construye a mano el mismo formato binario que stateSave() escribe
         * en plugin.cpp (ver kStateMagic/kStateVersion) e inyecta la ruta
         * del sample real via state->load - evita necesitar un drag-and-drop
         * o click real (sandbox sin input sintetico, ver memoria del
         * proyecto). */
        MemStream ms = {0};
        clap_ostream_t os = {.ctx = &ms, .write = mem_write};
        uint32_t magic = 0x53534C50, version = 22;
        int32_t rootNote = 60, volume = 0, bendUp = 2400, bendDown = -2400, quality = 1,
                loopModeIndex = 1, polyphony = 256, notePolyphony = 256,
                offsetValue = 0, ccVolume = 100, ccPan = 64, ampLfoWave = 1,
                ampKeycenter = 60, ampKeytrack = 0, ampVeltrack = 100, ampRandom = 0,
                panRandom = 0, panLfoPan = 0, panLfoWave = 1;
        float tuneCents = 0.f;
        float panLfoDelay = 0.0f, panLfoFade = 0.0f, panLfoFreq = 10.0f;
        uint8_t panAlternate = 0;
        float ampStartLevel = 0.0f, ampDelayTime = 0.00001f, ampAttackTime = 0.00001f,
             ampAttackShape = 0.00001f, ampHoldTime = 0.00001f, ampDecayTime = 0.00001f,
             ampDecayShape = -0.3616f, ampSustainLevel = 1.0f, ampReleaseTime = 0.00001f,
             ampReleaseShape = -6.3616f, ampLfoDelay = 0.0f, ampLfoFade = 0.0f,
             ampLfoVolume = 0.0f, ampLfoFreq = 10.0f;
        uint8_t offsetEnabled = 0, randomOffsetEnabled = 0;
        int32_t filterTypeIndex = 1, filterCutoff = 11700, filterRandomCutoff = 0,
                filKeycenter = 60, filKeytrack = 0, filVeltrack = 0, resoVeltrack = 0,
                filDepth = 0, filterLfoDepth = 0, filterLfoWave = 1;
        float filterResonance = 0.0f;
        float filEgStartLevel = 0.0f, filEgDelayTime = 0.00001f, filEgAttackTime = 0.00001f,
             filEgAttackShape = 0.00001f, filEgHoldTime = 0.00001f, filEgDecayTime = 0.00001f,
             filEgDecayShape = 0.00001f, filEgSustainLevel = 1.0f, filEgReleaseTime = 0.00001f,
             filEgReleaseShape = 0.00001f, filterLfoDelay = 0.0f, filterLfoFade = 0.0f,
             filterLfoFreq = 10.0f;
        uint8_t filterEgEnabled = 0;
        int32_t pitchKeytrack = 100, pitchVeltrack = 0, pitchRandom = 0, pitchDepth = 0,
                pitchLfoPitch = 0, pitchLfoWave = 1;
        float pitchEgStartLevel = 0.0f, pitchEgDelayTime = 0.00001f, pitchEgAttackTime = 0.00001f,
             pitchEgAttackShape = 0.00001f, pitchEgHoldTime = 0.00001f, pitchEgDecayTime = 0.00001f,
             pitchEgDecayShape = 0.00001f, pitchEgSustainLevel = 1.0f, pitchEgReleaseTime = 0.00001f,
             pitchEgReleaseShape = 0.00001f, pitchLfoDelay = 0.0f, pitchLfoFade = 0.0f,
             pitchLfoFreq = 10.0f;
        uint8_t pitchEgEnabled = 0;
        float glideTime = 0.0f;
        uint8_t portamentoEnabled = 0;
        uint8_t fxEnabled = 0;
        int32_t fxMode = 0, fxDetune = 0, fxDetuneCcMode = 0, fxDelayCcMode = 0,
                fxStereoWidth = 0, fxDepth = 0, fxWave = 1, fxPhaseCcMode = 0;
        float fxDelay = 0.0f, fxSpeed = 5.0f, fxPhase = 0.0f;
        uint8_t fxIndependentLfo = 0;
        int32_t fil2TypeIndex = 1, cutoff2 = 11700, fxVolume = -6;
        uint8_t reverbEnabled = 0;
        int32_t reverbTypeIndex = 0;
        float reverbInput = 100.0f, reverbPredelay = 50.0f, reverbSize = 50.0f,
             reverbTone = 50.0f, reverbDamp = 50.0f, reverbDry = 100.0f, reverbWet = 100.0f;
        const char* customOpcodes = "//mini_host_custom_opcode_marker";
        uint32_t customOpcodesLen = (uint32_t)strlen(customOpcodes);

        /* Sample tab "stack" (v21): mirrors state->load's expected layout
         * in plugin.cpp's stateLoad - a variable-length list of items
         * instead of the old fixed samplePath/multisampleEnabled/
         * multisamplePath/multisampleRegionsText fields. samplePath (if
         * given) becomes a plain-sample entry; multisampleRegionsFile (if
         * given) becomes an .sfz entry carrying a pre-flattened regions blob
         * (produced offline by SfzFlatten, same convention as the sample=
         * injection above bypassing drag-and-drop/click) - both can be
         * present at once, exercising the stack feature (a sample layered
         * with a multisample mapping) end to end. */
        const char* multisamplePath =
            multisamplePathOverride ? multisamplePathOverride
            : multisampleRegionsFile ? multisampleRegionsFile : "";
        uint32_t multisamplePathLen = (uint32_t)strlen(multisamplePath);
        char* multisampleRegionsText = NULL;
        uint32_t multisampleRegionsLen = 0;
        if (multisampleRegionsFile) {
            FILE* f = fopen(multisampleRegionsFile, "rb");
            if (!f) {
                fprintf(stderr, "no se pudo abrir multisample regions file: %s\n",
                       multisampleRegionsFile);
                return 1;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            multisampleRegionsText = (char*)malloc((size_t)sz);
            if (fread(multisampleRegionsText, 1, (size_t)sz, f) != (size_t)sz) {
                fprintf(stderr, "lectura incompleta de %s\n", multisampleRegionsFile);
                fclose(f);
                return 1;
            }
            fclose(f);
            multisampleRegionsLen = (uint32_t)sz;
        }
        mem_write(&os, &magic, 4);
        mem_write(&os, &version, 4);
        mem_write(&os, &rootNote, 4);
        mem_write(&os, &volume, 4);
        mem_write(&os, &bendUp, 4);
        mem_write(&os, &bendDown, 4);
        mem_write(&os, &quality, 4);
        mem_write(&os, &loopModeIndex, 4);
        mem_write(&os, &polyphony, 4);
        mem_write(&os, &notePolyphony, 4);
        mem_write(&os, &tuneCents, 4);
        mem_write(&os, &offsetEnabled, 1);
        mem_write(&os, &offsetValue, 4);
        mem_write(&os, &randomOffsetEnabled, 1);
        mem_write(&os, &panRandom, 4);
        mem_write(&os, &panAlternate, 1);
        mem_write(&os, &panLfoDelay, 4);
        mem_write(&os, &panLfoFade, 4);
        mem_write(&os, &panLfoPan, 4);
        mem_write(&os, &panLfoFreq, 4);
        mem_write(&os, &panLfoWave, 4);
        mem_write(&os, &ampKeycenter, 4);
        mem_write(&os, &ampKeytrack, 4);
        mem_write(&os, &ampVeltrack, 4);
        mem_write(&os, &ampRandom, 4);
        mem_write(&os, &ampStartLevel, 4);
        mem_write(&os, &ampDelayTime, 4);
        mem_write(&os, &ampAttackTime, 4);
        mem_write(&os, &ampAttackShape, 4);
        mem_write(&os, &ampHoldTime, 4);
        mem_write(&os, &ampDecayTime, 4);
        mem_write(&os, &ampDecayShape, 4);
        mem_write(&os, &ampSustainLevel, 4);
        mem_write(&os, &ampReleaseTime, 4);
        mem_write(&os, &ampReleaseShape, 4);
        mem_write(&os, &ampLfoDelay, 4);
        mem_write(&os, &ampLfoFade, 4);
        mem_write(&os, &ampLfoVolume, 4);
        mem_write(&os, &ampLfoFreq, 4);
        mem_write(&os, &ampLfoWave, 4);
        mem_write(&os, &filterTypeIndex, 4);
        mem_write(&os, &filterCutoff, 4);
        mem_write(&os, &filterResonance, 4);
        mem_write(&os, &filterRandomCutoff, 4);
        mem_write(&os, &filKeycenter, 4);
        mem_write(&os, &filKeytrack, 4);
        mem_write(&os, &filVeltrack, 4);
        mem_write(&os, &resoVeltrack, 4);
        mem_write(&os, &filterEgEnabled, 1);
        mem_write(&os, &filDepth, 4);
        mem_write(&os, &filEgStartLevel, 4);
        mem_write(&os, &filEgDelayTime, 4);
        mem_write(&os, &filEgAttackTime, 4);
        mem_write(&os, &filEgAttackShape, 4);
        mem_write(&os, &filEgHoldTime, 4);
        mem_write(&os, &filEgDecayTime, 4);
        mem_write(&os, &filEgDecayShape, 4);
        mem_write(&os, &filEgSustainLevel, 4);
        mem_write(&os, &filEgReleaseTime, 4);
        mem_write(&os, &filEgReleaseShape, 4);
        mem_write(&os, &filterLfoDelay, 4);
        mem_write(&os, &filterLfoFade, 4);
        mem_write(&os, &filterLfoDepth, 4);
        mem_write(&os, &filterLfoFreq, 4);
        mem_write(&os, &filterLfoWave, 4);
        mem_write(&os, &pitchKeytrack, 4);
        mem_write(&os, &pitchVeltrack, 4);
        mem_write(&os, &pitchRandom, 4);
        mem_write(&os, &portamentoEnabled, 1);
        mem_write(&os, &glideTime, 4);
        mem_write(&os, &pitchEgEnabled, 1);
        mem_write(&os, &pitchDepth, 4);
        mem_write(&os, &pitchEgStartLevel, 4);
        mem_write(&os, &pitchEgDelayTime, 4);
        mem_write(&os, &pitchEgAttackTime, 4);
        mem_write(&os, &pitchEgAttackShape, 4);
        mem_write(&os, &pitchEgHoldTime, 4);
        mem_write(&os, &pitchEgDecayTime, 4);
        mem_write(&os, &pitchEgDecayShape, 4);
        mem_write(&os, &pitchEgSustainLevel, 4);
        mem_write(&os, &pitchEgReleaseTime, 4);
        mem_write(&os, &pitchEgReleaseShape, 4);
        mem_write(&os, &pitchLfoDelay, 4);
        mem_write(&os, &pitchLfoFade, 4);
        mem_write(&os, &pitchLfoPitch, 4);
        mem_write(&os, &pitchLfoFreq, 4);
        mem_write(&os, &pitchLfoWave, 4);
        mem_write(&os, &ccVolume, 4);
        mem_write(&os, &ccPan, 4);
        mem_write(&os, &fxEnabled, 1);
        mem_write(&os, &fxMode, 4);
        mem_write(&os, &fxDetune, 4);
        mem_write(&os, &fxDetuneCcMode, 4);
        mem_write(&os, &fxDelay, 4);
        mem_write(&os, &fxDelayCcMode, 4);
        mem_write(&os, &fxStereoWidth, 4);
        mem_write(&os, &fxDepth, 4);
        mem_write(&os, &fxSpeed, 4);
        mem_write(&os, &fxWave, 4);
        mem_write(&os, &fxPhase, 4);
        mem_write(&os, &fxPhaseCcMode, 4);
        mem_write(&os, &fxIndependentLfo, 1);
        mem_write(&os, &fil2TypeIndex, 4);
        mem_write(&os, &cutoff2, 4);
        mem_write(&os, &fxVolume, 4);
        mem_write(&os, &reverbEnabled, 1);
        mem_write(&os, &reverbTypeIndex, 4);
        mem_write(&os, &reverbInput, 4);
        mem_write(&os, &reverbPredelay, 4);
        mem_write(&os, &reverbSize, 4);
        mem_write(&os, &reverbTone, 4);
        mem_write(&os, &reverbDamp, 4);
        mem_write(&os, &reverbDry, 4);
        mem_write(&os, &reverbWet, 4);
        mem_write(&os, &customOpcodesLen, 4);
        mem_write(&os, customOpcodes, customOpcodesLen);

        uint32_t stackCount = (samplePath ? 1u : 0u) + (multisampleRegionsFile ? 1u : 0u);
        mem_write(&os, &stackCount, 4);
        if (samplePath) {
            uint32_t pathLen = (uint32_t)strlen(samplePath);
            uint8_t isSfz = 0;
            uint32_t regionsLen = 0;
            mem_write(&os, &pathLen, 4);
            mem_write(&os, samplePath, pathLen);
            mem_write(&os, &isSfz, 1);
            mem_write(&os, &regionsLen, 4);
        }
        if (multisampleRegionsFile) {
            uint8_t isSfz = 1;
            mem_write(&os, &multisamplePathLen, 4);
            if (multisamplePathLen) mem_write(&os, multisamplePath, multisamplePathLen);
            mem_write(&os, &isSfz, 1);
            mem_write(&os, &multisampleRegionsLen, 4);
            if (multisampleRegionsLen) mem_write(&os, multisampleRegionsText, multisampleRegionsLen);
        }
        free(multisampleRegionsText);
        uint8_t mpeEnabled = 0; /* v17 */
        mem_write(&os, &mpeEnabled, 1);
        int32_t character = 0; /* v18 */
        mem_write(&os, &character, 4);
        int32_t multisampleRootNote = 60; /* v19 */
        mem_write(&os, &multisampleRootNote, 4);
        uint8_t panX2 = 0; /* v20 */
        mem_write(&os, &panX2, 1);
        /* v22: Pitch tab's Tuning section (DAW-session-only, not part of
         * .sspreset/.ssprofile - see shared.hpp's tuningFrequency comment).
         * Plain defaults here since this test doesn't exercise scala/tuning
         * itself, just regression-covers the format round-tripping. */
        mem_write(&os, &tuningFreqArg, 4);
        uint32_t scalaPathLen = scalaPathArg ? (uint32_t)strlen(scalaPathArg) : 0;
        mem_write(&os, &scalaPathLen, 4);
        if (scalaPathLen) mem_write(&os, scalaPathArg, scalaPathLen);

        clap_istream_t is = {.ctx = &ms, .read = mem_read};
        if (!state->load(plug, &is)) {
            fprintf(stderr, "state->load fallo (sample: %s, multisample: %s)\n",
                   samplePath ? samplePath : "(none)",
                   multisampleRegionsFile ? multisampleRegionsFile : "(none)");
            return 1;
        }
        printf("state->load OK (sample: %s, multisample: %s)\n",
              samplePath ? samplePath : "(none)",
              multisampleRegionsFile ? multisampleRegionsFile : "(none)");

        if (!plug->start_processing(plug)) {
            fprintf(stderr, "start_processing (2) fallo\n");
            return 1;
        }
        float maxAbs = 0.f;
        int zeroCrossings = 0;
        float prevL = 0.f;
        for (int b = 0; b < 20; ++b) {
            for (int i = 0; i < N; ++i) outL[i] = outR[i] = 0.f;
            evCtx.count = 0;
            if (b == 0) evCtx.events[evCtx.count++] = &noteOn.header;
            clap_process_status st = plug->process(plug, &proc);
            if (st == CLAP_PROCESS_ERROR) {
                fprintf(stderr, "process(2) devolvio error\n");
                return 1;
            }
            for (int i = 0; i < N; ++i) {
                if (outL[i] != outL[i] || outR[i] != outR[i]) {
                    fprintf(stderr, "NaN en la salida tras cargar el sample\n");
                    return 1;
                }
                float a = fabsf(outL[i]);
                if (a > maxAbs) maxAbs = a;
                a = fabsf(outR[i]);
                if (a > maxAbs) maxAbs = a;
                /* Zero-crossing count as a cheap pitch proxy (same
                 * technique used for the MPE bend-range verification) -
                 * proves sfizz_set_tuning_frequency actually shifts pitch,
                 * not just that state round-trips the value. */
                if ((prevL < 0.f && outL[i] >= 0.f) || (prevL > 0.f && outL[i] <= 0.f))
                    ++zeroCrossings;
                prevL = outL[i];
            }
        }
        /* Silence is only a real failure if the injected sample path was
         * actually loadable - deliberately testing a missing-file samplePath
         * (see plugin.cpp's stateLoad sampleError warning) is expected to
         * render silent, not a bug in that case. */
        int samplePathExists = samplePath && access(samplePath, F_OK) == 0;
        printf("render con sample real: maxAbs=%.6f zeroCrossings=%d (tuningfreq=%.2f) %s\n",
              (double)maxAbs, zeroCrossings, (double)tuningFreqArg,
              maxAbs > 1e-4f ? "(sonido detectado, OK)"
              : samplePathExists ? "(SILENCIO - posible fallo)"
                                 : "(SILENCIO esperado - samplePath no existe)");
        if (maxAbs <= 1e-4f && samplePathExists) return 1;
        plug->stop_processing(plug);

        /* roundtrip save -> load, para comprobar que save() tambien
         * funciona (no solo load con datos hechos a mano). */
        MemStream ms2 = {0};
        clap_ostream_t os2 = {.ctx = &ms2, .write = mem_write};
        if (!state->save(plug, &os2)) {
            fprintf(stderr, "state->save fallo\n");
            return 1;
        }
        printf("state->save OK (%llu bytes)\n", (unsigned long long)ms2.len);
        clap_istream_t is2 = {.ctx = &ms2, .read = mem_read};
        if (!state->load(plug, &is2)) {
            fprintf(stderr, "roundtrip save->load fallo\n");
            return 1;
        }
        printf("roundtrip save->load OK\n");
    }

    if (wantGui) {
        const clap_plugin_gui_t* gui =
            (const clap_plugin_gui_t*)plug->get_extension(plug, CLAP_EXT_GUI);
        if (gui && gui->is_api_supported(plug, CLAP_WINDOW_API_X11, false)) {
            if (!gui->create(plug, CLAP_WINDOW_API_X11, false)) {
                fprintf(stderr, "gui create fallo\n");
                return 1;
            }
            uint32_t w = 0, h = 0;
            gui->get_size(plug, &w, &h);
            gui->show(plug);
            printf("gui creada y visible (%ux%u), renderizando 4 s...\n", w, h);
            fflush(stdout);
            sleep(4);
            gui->destroy(plug);
            printf("gui destruida (OK)\n");
        }
    }

    plug->deactivate(plug);
    plug->destroy(plug);
    entry->deinit();
    printf("todo OK\n");
    return 0;
}
