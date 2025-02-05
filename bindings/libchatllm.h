#pragma once

#ifdef _WIN32
    #define API_CALL __stdcall
    #ifndef _WIN64
        #error unsupported target OS
    #endif
#elif __linux__
    #define API_CALL
    #if (!defined __x86_64__) && (!defined __aarch64__)
        #error unsupported target OS
    #endif
#else
    #define API_CALL
    #warning OS not supported, maybe
#endif

#ifndef DLL_DECL
#define DLL_DECL
#endif

#ifdef __cplusplus
extern "C"
{
#endif

enum PrintType
{
    PRINT_CHAT_CHUNK        = 0,
    // below items share the same value with BaseStreamer::TextType
    PRINTLN_META            = 1,    // print a whole line: general information
    PRINTLN_ERROR           = 2,    // print a whole line: error message
    PRINTLN_REF             = 3,    // print a whole line: reference
    PRINTLN_REWRITTEN_QUERY = 4,    // print a whole line: rewritten query
    PRINTLN_HISTORY_USER    = 5,    // print a whole line: user input history
    PRINTLN_HISTORY_AI      = 6,    // print a whole line: AI output history
    PRINTLN_TOOL_CALLING    = 7,    // print a whole line: tool calling (supported by only a few models)
    PRINTLN_EMBEDDING       = 8,    // print a whole line: embedding (example: "0.1,0.3,...")
    PRINTLN_RANKING         = 9,    // print a whole line: ranking (example: "0.8")
    PRINTLN_TOKEN_IDS       =10,    // print a whole line: token ids (example: "1,3,5,8,...")
    PRINTLN_LOGGING         =11,    // print a whole line: internal logging with the first char indicating level
                                    // (space): None; D: Debug; I: Info; W: Warn; E: Error; .: continue
    PRINTLN_BEAM_SEARCH     =12,    // print a whole line: a result of beam search with a prefix of probability
                                    // (example: "0.8,....")

    PRINT_EVT_ASYNC_COMPLETED  = 100,   // last async operation completed (utf8_str is null)
};

typedef void (*f_chatllm_print)(void *user_data, int print_type, const char *utf8_str);
typedef void (*f_chatllm_end)(void *user_data);

struct chatllm_obj;

/**
 * Usage:
 *
 * ```c
 * obj = create(callback functions);
 * append_param(obj, ...);
 * // ...
 * app_param(obj, ...);
 *
 * start(obj);
 * while (true)
 * {
 *     user_input(obj, ...);
 * }
 * ```
*/

/**
 * @brief create ChatLLM object
 *
 * @return                  the object
 */
DLL_DECL struct chatllm_obj * API_CALL chatllm_create(void);

/**
 * @brief append a command line option
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          a command line option
 */
DLL_DECL void API_CALL chatllm_append_param(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief start
 *
 * @param[in] obj               model object
 * @param[in] f_print           callback function for printing
 * @param[in] f_end             callback function when model generation ends
 * @param[in] user_data         user data provided to callback functions
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_start(struct chatllm_obj *obj, f_chatllm_print f_print, f_chatllm_end f_end, void *user_data);

/**
 * @brief set max number of generated tokens in a new round of conversation
 *
 * @param[in] obj               model object
 * @param[in] gen_max_tokens    -1 for as many as possible
 */
DLL_DECL void API_CALL chatllm_set_gen_max_tokens(struct chatllm_obj *obj, int gen_max_tokens);

/**
 * @brief restart (i.e. discard history)
 *
 * * When a session has been loaded, the model is restarted to the point that the session is loaded;
 *
 *      Note: this would not work if `--extending` is not `none` or the model uses SWA.
 *
 * * Otherwise, it is restarted from the very beginning.
 *
 * @param[in] obj               model object
 * @param[in] utf8_sys_prompt   update to a new system prompt
 *                              if NULL, then system prompt is kept unchanged.
 */
DLL_DECL void API_CALL chatllm_restart(struct chatllm_obj *obj, const char *utf8_sys_prompt);

/**
 * @brief user input
 *
 * This function is synchronized, i.e. it returns after model generation ends and `f_end` is called.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          user input
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_user_input(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief set prefix for AI generation
 *
 * This prefix is used in all following rounds..
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          prefix
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_set_ai_prefix(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief add a suffix to ai output and continue generation
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          suffix
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_ai_continue(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief tool input
 *
 * - If this function is called before `chatllm_user_input` returns, this is asynchronized,
 * - If this function is called after `chatllm_user_input` returns, this is equivalent to
 *   `chatllm_user_input`.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          user input
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_tool_input(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief feed in text generated by external tools
 *
 * This text is treated as part of AI's generation. After this is called, LLM generation
 * is continued.
 *
 * Example:
 *
 * ```c
 * // in `f_print` callback:
 * chatllm_abort_generation();
 * chatllm_tool_completion(...);
 * ```
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          text
 * @return                      0 if succeeded
 */
DLL_DECL int chatllm_tool_completion(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief tokenize
 *
 * token ids are emitted through `PRINTLN_TOKEN_IDS`.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          text
 * @return                      number of ids if succeeded. otherwise -1.
 */
DLL_DECL int chatllm_text_tokenize(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief text embedding
 *
 * embedding is emitted through `PRINTLN_EMBEDDING`.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          text
 * @return                      0 if succeeded
 */
DLL_DECL int chatllm_text_embedding(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief question & answer ranking
 *
 * embedding is emit through `PRINTLN_RANKING`.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str_q        question
 * @param[in] utf8_str_q        answer
 * @return                      0 if succeeded
 */
DLL_DECL int chatllm_qa_rank(struct chatllm_obj *obj, const char *utf8_str_q, const char *utf8_str_a);

/**
 * @brief switching RAG vector store
 *
 * @param[in] obj               model object
 * @param[in] name              vector store name
 * @return                      0 if succeeded
 */
DLL_DECL int chatllm_rag_select_store(struct chatllm_obj *obj, const char *name);

/**
 * @brief abort generation
 *
 * This function is asynchronized, i.e. it returns immediately.
 *
 * @param[in] obj               model object
 */
DLL_DECL void API_CALL chatllm_abort_generation(struct chatllm_obj *obj);

/**
 * @brief show timing statistics
 *
 * Result is sent to `f_print`.
 *
 * @param[in] obj               model object
 */
DLL_DECL void API_CALL chatllm_show_statistics(struct chatllm_obj *obj);

/**
 * @brief save current session on demand
 *
 * Note: Call this from the same thread of `chatllm_user_input()`.
 *
 * If chat history is empty, then system prompt is evaluated and saved.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          file full name
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_save_session(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief load a session on demand
 *
 * Note: Call this from the same thread of `chatllm_user_input()`.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          file full name
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_load_session(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief get integer result of last async operation
 *
 * @param[in] obj               model object
 * @return                      last result (if async is still ongoing, INT_MIN)
 */
DLL_DECL int API_CALL chatllm_get_async_result_int(struct chatllm_obj *obj);

/**
 * @brief async version of `chatllm_start`
 *
 * @param   ...
 * @return                      0 if started else -1
 */
DLL_DECL int API_CALL chatllm_async_start(struct chatllm_obj *obj, f_chatllm_print f_print, f_chatllm_end f_end, void *user_data);

/**
 * @brief async version of `chatllm_user_input`

 * @param   ...
 * @return                      0 if started else -1
 */
DLL_DECL int API_CALL chatllm_async_user_input(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief async version of `chatllm_tool_input`

 * @param   ...
 * @return                      0 if started else -1
 */
DLL_DECL int API_CALL chatllm_async_tool_input(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief async version of `chatllm_tool_completion`

 * @param   ...
 * @return                      0 if started else -1
 */
DLL_DECL int chatllm_async_tool_completion(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief async version of `chatllm_text_embedding`

 * @param   ...
 * @return                      0 if started else -1
 */
DLL_DECL int chatllm_async_text_embedding(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief async version of `chatllm_qa_rank`

 * @param   ...
 * @return                      0 if started else -1
 */
DLL_DECL int chatllm_async_qa_rank(struct chatllm_obj *obj, const char *utf8_str_q, const char *utf8_str_a);

#ifdef __cplusplus
}
#endif