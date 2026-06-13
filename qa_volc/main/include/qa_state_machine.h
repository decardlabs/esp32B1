#ifndef QA_STATE_MACHINE_H
#define QA_STATE_MACHINE_H

typedef enum {
    QA_STATE_BOOT = 0,
    QA_STATE_WIFI_CONNECT,
    QA_STATE_IDLE,
    QA_STATE_RECORDING,
    QA_STATE_ASR_WAIT,
    QA_STATE_LLM_WAIT,
    QA_STATE_ERROR,
} qa_state_t;

const char *qa_state_name(qa_state_t state);
#endif
