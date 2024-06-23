#include <epan/packet.h>
#include "tracee.h"

static dissector_handle_t event_postdissector;

wmem_map_t *event_postdissectors;

static int dissect_sched_process_exec(tvbuff_t *tvb _U_, packet_info *pinfo, proto_tree *tree _U_, void *data _U_)
{
    const gchar *pathname, *cmdline;

    pathname = wanted_field_get_str("tracee.args.pathname");
    cmdline = wanted_field_get_str("tracee.args.command_line");

    if (pathname && cmdline) {
        if (strncmp(pathname, cmdline, strlen(pathname)) == 0)
            col_add_str(pinfo->cinfo, COL_INFO, cmdline);
        else
            col_add_fstr(pinfo->cinfo, COL_INFO, "%s: %s", pathname, cmdline);
    }

    return 0;
}

static int dissect_net_packet_http_request(tvbuff_t *tvb _U_, packet_info *pinfo, proto_tree *tree _U_, void *data _U_)
{
    const gchar *method, *protocol, *uri_path, *content_type;

    method = wanted_field_get_str("tracee.proto_http_request.method");
    protocol = wanted_field_get_str("tracee.proto_http_request.protocol");
    uri_path = wanted_field_get_str("tracee.proto_http_request.uri_path");

    if (!method || !protocol || !uri_path)
        return 0;
    
    col_add_fstr(pinfo->cinfo, COL_INFO, "%s %s %s", method, uri_path, protocol);
    
    if (strcmp(method, "POST") && ((content_type = wanted_field_get_str("http.content_type")) != NULL))
        col_append_fstr(pinfo->cinfo, COL_INFO, "  (%s)", content_type);

    return 0;
}

static int dissect_net_packet_http(tvbuff_t *tvb _U_, packet_info *pinfo, proto_tree *tree _U_, void *data _U_)
{
    const gchar *direction, *method, *protocol, *uri_path, *content_type, *status;
    gchar *tmp, *content_type_short = NULL;
    gboolean request;

    direction = wanted_field_get_str("tracee.proto_http.direction");
    if (direction == NULL)
        return 0;
    if (strcmp(direction, "request") == 0)
        request = TRUE;
    else if (strcmp(direction, "response") == 0)
        request = FALSE;
    else
        return 0;
    
    protocol = wanted_field_get_str("tracee.proto_http.protocol");
    content_type = wanted_field_get_str("http.content_type");

    // discard semicolon in content type
    if (content_type != NULL) {
        content_type_short = wmem_strdup(pinfo->pool, content_type);
        tmp = strchr(content_type_short, ';');
        if (tmp != NULL)
            *tmp = '\0';
    }

    if (request) {
        method = wanted_field_get_str("tracee.proto_http.method");
        uri_path = wanted_field_get_str("tracee.proto_http.uri_path");

        if (!method || !protocol || !uri_path)
            return 0;
        
        col_add_fstr(pinfo->cinfo, COL_INFO, "%s %s %s", method, uri_path, protocol);
        
        if (content_type_short != NULL)
            col_append_fstr(pinfo->cinfo, COL_INFO, " (%s)", content_type_short);
    }

    else {
        status = wanted_field_get_str("tracee.proto_http.status");

        if (!protocol || !status)
            return 0;
        
        col_add_fstr(pinfo->cinfo, COL_INFO, "%s %s", protocol, status);

        if (content_type_short != NULL)
            col_append_fstr(pinfo->cinfo, COL_INFO, "  (%s)", content_type_short);
    }

    return 0;
}

static int dissect_security_socket_bind_connect(packet_info *pinfo, const gchar *verb)
{
    const gchar *family, *addr, *port;

    family = wanted_field_get_str("tracee.sockaddr.sa_family");

    if (family == NULL)
        return 0;
    
    if (strcmp(family, "AF_INET") == 0) {
        addr = wanted_field_get_str("tracee.sockaddr.sin_addr");
        port = wanted_field_get_str("tracee.sockaddr.sin_port");
    }
    else if (strcmp(family, "AF_INET6") == 0) {
        addr = wanted_field_get_str("tracee.sockaddr.sin6_addr");
        port = wanted_field_get_str("tracee.sockaddr.sin6_port");
    }
    else
        return 0;
    
    if (addr && port) {
        col_add_fstr(pinfo->cinfo, COL_INFO, "%s to %s port %s", verb, addr, port);
    }

    return 0;
}

static int dissect_security_socket_bind(tvbuff_t *tvb _U_, packet_info *pinfo, proto_tree *tree _U_, void *data _U_)
{
    return dissect_security_socket_bind_connect(pinfo, "Bind");
}

static int dissect_security_socket_connect(tvbuff_t *tvb _U_, packet_info *pinfo, proto_tree *tree _U_, void *data _U_)
{
    return dissect_security_socket_bind_connect(pinfo, "Connect");
}

static int dissect_dynamic_code_loading(tvbuff_t *tvb _U_, packet_info *pinfo, proto_tree *tree _U_, void *data _U_)
{
    const gchar *alert = wanted_field_get_str("tracee.args.alert");

    if (alert)
        col_append_str(pinfo->cinfo, COL_INFO, alert);
    
    return 0;
}

static int dissect_fileless_execution(tvbuff_t *tvb _U_, packet_info *pinfo, proto_tree *tree _U_, void *data _U_)
{
    const gchar *pathname = wanted_field_get_str("tracee.args.pathname");

    if (pathname)
        col_append_fstr(pinfo->cinfo, COL_INFO, "Running from %s", pathname);
    
    return 0;
}

static int dissect_stdio_over_socket(tvbuff_t *tvb _U_, packet_info *pinfo, proto_tree *tree _U_, void *data _U_)
{
    gint *fd;
    const gchar *addr, *port, *stream;

    fd = wanted_field_get_int("tracee.args.File_descriptor");
    addr = wanted_field_get_str("tracee.args.IP_address");
    port = wanted_field_get_str("tracee.args.Port");

    if (fd && addr && port) {
        switch (*fd) {
            case 0:
                stream = "STDIN";
                break;
            case 1:
                stream = "STDOUT";
                break;
            case 2:
                stream = "STDERR";
                break;
            default:
                return 0;
        }

        col_add_fstr(pinfo->cinfo, COL_INFO, "%s forwarded to %s port %s", stream, addr, port);
    }

    return 0;
}

static int postdissect_event(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    const char *event_name;
    dissector_t dissector;

    event_name = wanted_field_get_str("tracee.eventName");
    if (event_name == NULL)
        return 0;
    
    if ((dissector = wmem_map_lookup(event_postdissectors, event_name)) == NULL)
        return 0;
    
    return dissector(tvb, pinfo, tree, data);
}

static void register_tracee_event_postdissector(const gchar *event_name, dissector_t dissector_func)
{
    wmem_map_insert(event_postdissectors, event_name, dissector_func);
}

static void register_wanted_fields(void)
{
    // needed for general postdissector
    register_wanted_field("tracee.eventName");

    // needed for dissect_sched_process_exec and dissect_fileless_execution
    register_wanted_field("tracee.args.pathname");
    register_wanted_field("tracee.args.command_line");

    // needed for dissect_net_packet_http_request
    register_wanted_field("tracee.proto_http_request.method");
    register_wanted_field("tracee.proto_http_request.protocol");
    register_wanted_field("tracee.proto_http_request.uri_path");
    register_wanted_field("http.content_type");

    // needed for dissect_net_packet_http
    register_wanted_field("tracee.proto_http.direction");
    register_wanted_field("tracee.proto_http.method");
    register_wanted_field("tracee.proto_http.protocol");
    register_wanted_field("tracee.proto_http.uri_path");
    register_wanted_field("tracee.proto_http.status");

    // needed for dissect_security_socket_bind_connect
    register_wanted_field("tracee.sockaddr.sa_family");
    register_wanted_field("tracee.sockaddr.sin_addr");
    register_wanted_field("tracee.sockaddr.sin_port");
    register_wanted_field("tracee.sockaddr.sin6_addr");
    register_wanted_field("tracee.sockaddr.sin6_port");

    // needed for dissect_dynamic_code_loading
    register_wanted_field("tracee.args.alert");

    // needed for dissect_stdio_over_socket
    register_wanted_field("tracee.args.File_descriptor");
    register_wanted_field("tracee.args.IP_address");
    register_wanted_field("tracee.args.Port");
}

void register_tracee_postdissectors(int proto)
{
    event_postdissector = register_dissector("tracee-event-postdissector", postdissect_event, proto);
    register_postdissector(event_postdissector);

    event_postdissectors = wmem_map_new(wmem_epan_scope(), g_str_hash, g_str_equal);

    register_tracee_event_postdissector("sched_process_exec", dissect_sched_process_exec);
    register_tracee_event_postdissector("net_packet_http_request", dissect_net_packet_http_request);
    register_tracee_event_postdissector("net_packet_http", dissect_net_packet_http);
    register_tracee_event_postdissector("security_socket_bind", dissect_security_socket_bind);
    register_tracee_event_postdissector("security_socket_connect", dissect_security_socket_connect);
    register_tracee_event_postdissector("dynamic_code_loading", dissect_dynamic_code_loading);
    register_tracee_event_postdissector("fileless_execution", dissect_fileless_execution);
    register_tracee_event_postdissector("stdio_over_socket", dissect_stdio_over_socket);

    register_wanted_fields();
}