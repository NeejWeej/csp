import logging
import math
import threading
import urllib
from collections import defaultdict
from datetime import date, datetime, timedelta
from typing import Dict, List, Optional, TypeVar, Union

import csp
from csp import ts
from csp.adapters.dynamic_adapter_utils import AdapterInfo
from csp.adapters.status import Status
from csp.adapters.utils import (
    BytesMessageProtoMapper,
    DateTimeType,
    JSONTextMessageMapper,
    MsgMapper,
    RawBytesMessageMapper,
    RawTextMessageMapper,
)
from csp.adapters.websocket_types import (
    ActionType,
    ConnectionRequest,
    InternalConnectionRequest,
    WebsocketHeaderUpdate,
    WebsocketStatus,
)
from csp.impl.wiring import input_adapter_def, output_adapter_def, status_adapter_def
from csp.impl.wiring.delayed_node import DelayedNodeWrapperDef
from csp.lib import _websocketadapterimpl

# InternalConnectionRequest,
_ = (
    ActionType,
    BytesMessageProtoMapper,
    DateTimeType,
    JSONTextMessageMapper,
    RawBytesMessageMapper,
    RawTextMessageMapper,
    WebsocketStatus,
)
T = TypeVar("T")


try:
    import tornado.ioloop
    import tornado.web
    import tornado.websocket
except ImportError:
    raise ImportError("websocket adapter requires tornado package")

try:
    import rapidjson

    datetime_mode = rapidjson.DM_UNIX_TIME | rapidjson.DM_NAIVE_IS_UTC
except ImportError:
    raise ImportError("websocket adapter requires rapidjson package")


def diff_dict(old, new):
    d = {}
    for k, v in new.items():
        oldv = old.get(k, csp.UNSET)

        if v != oldv and (not isinstance(oldv, float) or not math.isnan(v) or not math.isnan(oldv)):
            d[k] = v

    return d


def _sanitize_port(uri: str, port):
    if port:
        return str(port)
    return "443" if uri.startswith("wss") else "80"


class TableManager:
    def __init__(self, tables, delta_updates):
        self._tables = tables
        self._snapshots = {}
        for name, table in tables.items():
            self._snapshots[name] = defaultdict(dict) if table.index else []
        self._subscriptions = defaultdict(set)
        self._delta_updates = delta_updates

    def __iter__(self):
        return iter(self._tables.values())

    def __contains__(self, name):
        return name in self._tables

    def subscribe(self, table, handler):
        sub = self._subscriptions[table]
        sub.add(handler)

    def unsubscribe(self, handler):
        for sub in self._subscriptions.values():
            if handler in sub:
                sub.remove(handler)

    def send_updates(self, tablename, data):
        # Update snapshot
        table = self._tables[tablename]

        snapshot = self._snapshots[tablename]

        if table.index is not None:
            index = data[table.index]
            old_data = snapshot[index]
            data = diff_dict(old_data, data)
            data[table.index] = index
            old_data.update(data)
        else:
            snapshot.append(data)

        subs = self._subscriptions[tablename]
        if len(subs):
            logging.debug("sending message to {num} subscribers".format(num=len(subs)))
            msg = {"messageType": "upd", "data": [data]}

            for handler in list(subs):
                try:
                    handler.send(msg)
                except tornado.websocket.WebSocketClosedError:
                    # In process of shutting down
                    subs.remove(handler)
                    logging.error("Error sending message", exc_info=True)

    def get_snapshot(self, tablename):
        return self._snapshots[tablename]


class BaseRequestHandler(tornado.web.RequestHandler):
    def initialize(self, manager):
        self._manager = manager

    def write_error(self, status_code, **kwargs):
        if "exc_info" in kwargs:
            exc = kwargs["exc_info"]
            self.finish(exc[1].log_message)

    def set_default_headers(self):
        self.set_header("Content-Type", b"application/json")
        # Support CORS
        self.set_header("Access-Control-Allow-Origin", "*")
        self.set_header("Access-Control-Allow-Methods", "GET, OPTIONS")

    def options(self):
        # no body
        self.set_status(204)
        self.finish()


class TableSnapRequestHandler(BaseRequestHandler):
    def get(self, table):
        if table not in self._manager:
            raise tornado.web.HTTPError(404, "Invalid table requested")

        # Send snapshot
        snapshot = self._manager.get_snapshot(table)
        data = {"snap": list(snapshot.values())}

        resp = rapidjson.dumps(data, datetime_mode=datetime_mode)
        self.write(resp)
        self.finish()


class TablesRequestHandler(BaseRequestHandler):
    def get(self):
        # Send list of tables
        sub = urllib.parse.urlunparse(("ws", self.request.host, "/subscribe/{table}", "", "", ""))

        data = {
            "tables": [
                {
                    "name": table.name,
                    "index": table.index,
                    "schema": table.schema,
                    "sub": sub.format(table=table.name),
                }
                for table in self._manager
            ],
        }

        resp = rapidjson.dumps(data, datetime_mode=datetime_mode)
        self.write(resp)
        self.finish()


class WebSocketHandler(tornado.websocket.WebSocketHandler):
    def initialize(self, manager):
        self._manager = manager
        self._seqid = 0

    def prepare(self):
        if len(self.path_args) == 1:
            table = self.path_args[0]

            if table not in self._manager:
                raise tornado.web.HTTPError(404, "Invalid table requested")
        else:
            raise tornado.web.HTTPError(404)

    def check_origin(self, origin):
        # No leakage outside of internal domain
        parsed = urllib.parse.urlparse(origin)

        if parsed.scheme == "file":
            return True

        return True
        # FIXME
        # host = parsed.hostname.split(".")
        # return (len(host) == 1) or parsed.hostname.endswith("YOUR INTERNAL DOMAIN")

    def get_compression_options(self):
        # Non-None enables compression with default options.
        return {}

    def send(self, data: dict):
        # Add sequence id to message
        data["messageID"] = self._seqid
        self._seqid += 1

        # Send data
        resp = rapidjson.dumps(data, datetime_mode=datetime_mode).replace("NaN", "null")
        self.write_message(resp)

    def open(self, table):
        remote = self.request.remote_ip
        logging.info(f"New connection from {remote} for '{table}'")

        snapshot = self._manager.get_snapshot(table)
        snapshots = list(snapshot.values()) if isinstance(snapshot, dict) else snapshot
        MAX_RECORDS = 100
        start_idx = 0
        while start_idx < len(snapshots):
            msg = {
                "messageType": "snap",
                "data": snapshots[start_idx : start_idx + MAX_RECORDS],
            }
            self.send(msg)
            start_idx += MAX_RECORDS

        # Subscribe to updates
        self._manager.subscribe(table, self)

    def on_close(self):
        remote = self.request.remote_ip
        logging.info(f"Connection closed from {remote}")
        # Clean up subscriptions
        self._manager.unsubscribe(self)

    def on_message(self, message):
        logging.warning("got message %r", message)
        # TODO Ignore for now
        # parsed = rapidjson.loads(message)


class _BatchedUpdate:
    """ioloop add_callback can get expensive if called many many times, this seems to be considerably faster
    only schedule if needed / queue is empty"""

    def __init__(self, manager, table, ioloop):
        self._queue = []
        self._lock = threading.Lock()
        self._manager = manager
        self._table = table
        self._ioloop = ioloop

    def send_update(self, data):
        with self._lock:
            self._queue.append(data)
            needs_schedule = len(self._queue) == 1

        if needs_schedule:
            self._ioloop.add_callback(self.process_queue)

    def process_queue(self):
        with self._lock:
            localq = self._queue
            self._queue = []

        for data in localq:
            self._manager.send_updates(self._table, data)


@csp.node
def _apply_updates(manager: TableManager, table: str, data: {str: ts[object]}):
    """One node per publishing table"""
    with csp.state():
        s_batch_update = _BatchedUpdate(manager, table, tornado.ioloop.IOLoop.current())

    if csp.ticked(data):
        s_batch_update.send_update(dict(data.tickeditems()))


@csp.node
def _launch_application(port: int, manager: object, stub: ts[object]):
    with csp.state():
        s_app = None
        s_ioloop = None
        s_iothread = None

    with csp.start():
        s_app = tornado.web.Application(
            [
                (r"/tables", TablesRequestHandler, {"manager": manager}),
                (r"/snap/(.*)", TableSnapRequestHandler, {"manager": manager}),
                (r"/subscribe/(.*)", WebSocketHandler, {"manager": manager}),
            ],
            websocket_ping_interval=15,
        )

        s_app.listen(port)
        s_ioloop = tornado.ioloop.IOLoop.current()
        s_iothread = threading.Thread(target=s_ioloop.start)
        s_iothread.start()

    with csp.stop():
        if s_ioloop:
            s_ioloop.add_callback(s_ioloop.stop)
            if s_iothread:
                s_iothread.join()


class TableAdapter:
    typemap = {
        str: "string",
        float: "float",
        int: "integer",
        bool: "boolean",
        date: "date",
        datetime: "datetime",
    }

    """ dont create these directly, use WebsocketAdapter """

    def __init__(self, name, index):
        self.name = name
        self.index = index
        self.columns = {}
        self.schema = {}

    def publish(
        self,
        value: ts[object],
        field_map: Union[Dict[str, str], str, None] = None,
    ):
        """
        :param value - timeseries to publish onto this table
        :param field_map: if publishing structs, a dictionary of struct field -> perspective fieldname ( if None will pass struct fields as is )
                          if publishing a single field, then string name of the destination column
        """
        if issubclass(value.tstype.typ, csp.Struct):
            self._publish_struct(value, field_map)
        else:
            if not isinstance(field_map, str):
                raise TypeError("Expected type str for field_map on single column publish, got %s" % type(field_map))
            self._publish_field(value, field_map)

    def _publish_struct(self, value: ts[csp.Struct], field_map: Optional[Dict[str, str]]):
        field_map = field_map or {k: k for k in value.tstype.typ.metadata()}
        for k, v in field_map.items():
            self._publish_field(getattr(value, k), v)

    def _publish_field(self, value: ts[object], column_name: str):
        if column_name in self.columns:
            raise KeyError(f"Trying to add column {column_name} more than once")
        self.columns[column_name] = value
        self.schema[column_name] = TableAdapter.typemap[value.tstype.typ]


class WebsocketTableAdapter(DelayedNodeWrapperDef):
    def __init__(self, port, delta_updates=False):
        """
        :param port:          socket port to listen on
        :param delta_updates: for indexed tables, only send delta of fields that changed after initial snapshot
        """
        super().__init__()
        self._port = port
        self._tables = {}
        self._delta_updates = delta_updates

    def copy(self):
        res = WebsocketTableAdapter(self._port)
        res._tables.update(self._tables)
        return res

    def create_table(self, name, index=None):
        if name in self._tables:
            raise ValueError(f"Table {name} already exists")

        table = self._tables[name] = TableAdapter(name, index)
        return table

    def _instantiate(self):
        manager = TableManager(self._tables, self._delta_updates)
        for table_name, table in self._tables.items():
            _apply_updates(manager, table_name, table.columns)

        _launch_application(self._port, manager, csp.const("stub"))


class WebsocketAdapterManager:
    """
    Can subscribe dynamically via ts[List[ConnectionRequest]]

    We use a ts[List[ConnectionRequest]] to allow users to submit a batch of conneciton requests in
    a single engine cycle.
    """

    def __init__(
        self,
        uri: Optional[str] = None,
        reconnect_interval: timedelta = timedelta(seconds=2),
        headers: Optional[Dict[str, str]] = None,
        dynamic: bool = False,
        connection_request: Optional[ConnectionRequest] = None,
        num_threads: int = 1,
        binary: bool = False,
    ):
        """
        uri: str
            where to connect
        reconnect_interval: timedelta = timedelta(seconds=2)
            time interval to wait before trying to reconnect (must be >= 1 second)
        headers: Dict[str, str] = None
            headers to apply to the request during the handshake
        dynamic: bool = False
            Whether we accept dynamically altering the connections via ConnectionRequest objects.
        num_threads: int = 1
            Determines number of threads to allocate for running the websocket endpoints.
            Defaults to 1 to avoid thread switching
        binary: bool = False
            Whether to send/receive text or binary data
        """

        self._properties = dict(dynamic=dynamic, num_threads=num_threads, binary=binary)
        # Enumerating for clarity
        if connection_request is not None and uri is not None:
            raise ValueError("'connection_request' cannot be set along with 'uri'")

        # Exactly 1 of connection_request and uri is None
        if connection_request is not None or uri is not None:
            if connection_request is None:
                connection_request = ConnectionRequest(
                    uri=uri, reconnect_interval=reconnect_interval, headers=headers or {}
                )
            self._properties.update(self._get_properties(connection_request).to_dict())

        # This is a counter that will be used to identify every function call
        # We keep track of the subscribes and sends separately
        self._subscribe_call_id = 0
        self._send_call_id = 0

        # This maps types to their wrapper structs
        self._wrapper_struct_dict = {}

    @property
    def _dynamic(self):
        return self._properties.get("dynamic", False)

    def _get_properties(self, conn_request: ConnectionRequest) -> InternalConnectionRequest:
        uri = conn_request.uri
        reconnect_interval = conn_request.reconnect_interval

        assert reconnect_interval >= timedelta(seconds=1)
        resp = urllib.parse.urlparse(uri)
        if resp.hostname is None:
            raise ValueError(f"Failed to parse host from URI: {uri}")

        res = InternalConnectionRequest(
            host=resp.hostname,
            # if no port is explicitly present in the uri, the resp.port is None
            port=_sanitize_port(uri, resp.port),
            route=resp.path or "/",  # resource shouldn't be empty string
            use_ssl=uri.startswith("wss"),
            reconnect_interval=reconnect_interval,
            headers=rapidjson.dumps(conn_request.headers) if conn_request.headers else "",
            persistent=conn_request.persistent,
            action=conn_request.action.name,
            on_connect_payload=conn_request.on_connect_payload,
            uri=uri,
            dynamic=self._dynamic,
            binary=self._properties.get("binary", False),
        )
        return res

    def _get_caller_id(self, is_subscribe: bool) -> int:
        if is_subscribe:
            caller_id = self._subscribe_call_id
            self._subscribe_call_id += 1
        else:
            caller_id = self._send_call_id
            self._send_call_id += 1
        return caller_id

    def get_wrapper_struct(self, ts_type: type):
        if (dynamic_type := self._wrapper_struct_dict.get(ts_type)) is None:
            # I want to preserve type information
            # Not sure a better way to do this
            class CustomWrapperStruct(csp.Struct):
                msg: ts_type  #  noqa
                uri: str

            dynamic_type = CustomWrapperStruct
            self._wrapper_struct_dict[ts_type] = dynamic_type
        return dynamic_type

    def subscribe(
        self,
        ts_type: type,
        msg_mapper: MsgMapper,
        field_map: Union[dict, str] = None,
        meta_field_map: dict = None,
        push_mode: csp.PushMode = csp.PushMode.NON_COLLAPSING,
        connection_request: Optional[ts[List[ConnectionRequest]]] = None,
    ):
        """If dynamic is True, this will tick a custom WrapperStruct,
        with 'msg' as the correct type of the message.
        And 'uri' that specifies the 'uri' the message comes from.

        Otherwise, returns just message.

        ts_type should be original type!! The tuple wrapping happens
        automatically
        """
        caller_id = self._get_caller_id(is_subscribe=True)
        # Gives validation, more to start defining a common interface
        adapter_props = AdapterInfo(caller_id=caller_id, is_subscribe=True).model_dump()
        connection_request = csp.null_ts(List[ConnectionRequest]) if connection_request is None else connection_request
        request_dict = csp.apply(
            connection_request,
            lambda conn_reqs: [self._get_properties(conn_req) for conn_req in conn_reqs],
            List[InternalConnectionRequest],
        )
        # Output adapter to handle connection requests
        _websocket_connection_request_adapter_def(self, request_dict, adapter_props)

        field_map = field_map or {}
        meta_field_map = meta_field_map or {}
        if isinstance(field_map, str):
            field_map = {field_map: ""}

        if not field_map and issubclass(ts_type, csp.Struct):
            field_map = ts_type.default_field_map()

        properties = msg_mapper.properties.copy()
        properties["field_map"] = field_map
        properties["meta_field_map"] = meta_field_map

        properties.update(adapter_props)
        # We wrap the message in a struct to note the url it comes from
        if self._dynamic:
            ts_type = self.get_wrapper_struct(ts_type=ts_type)
        return _websocket_input_adapter_def(self, ts_type, properties, push_mode=push_mode)

    def send(self, x: ts["T"], connection_request: Optional[ts[List[ConnectionRequest]]] = None):
        caller_id = self._get_caller_id(is_subscribe=False)
        # Gives validation, more to start defining a common interface
        adapter_props = AdapterInfo(caller_id=caller_id, is_subscribe=False).model_dump()
        connection_request = csp.null_ts(List[ConnectionRequest]) if connection_request is None else connection_request
        request_dict = csp.apply(
            connection_request,
            lambda conn_reqs: [self._get_properties(conn_req) for conn_req in conn_reqs],
            List[InternalConnectionRequest],
        )
        _websocket_connection_request_adapter_def(self, request_dict, adapter_props)
        return _websocket_output_adapter_def(self, x, adapter_props)

    def update_headers(self, x: ts[List[WebsocketHeaderUpdate]]):
        if self._dynamic:
            raise ValueError("If dynamic, cannot call update_headers")
        return _websocket_header_update_adapter_def(self, x)

    def status(self, push_mode=csp.PushMode.NON_COLLAPSING):
        ts_type = Status
        return status_adapter_def(self, ts_type, push_mode)

    def _create(self, engine, memo):
        """method needs to return the wrapped c++ adapter manager"""
        self._properties.update({"subscribe_calls": self._subscribe_call_id, "send_calls": self._send_call_id})
        return _websocketadapterimpl._websocket_adapter_manager(engine, self._properties)


_websocket_input_adapter_def = input_adapter_def(
    "websocket_input_adapter",
    _websocketadapterimpl._websocket_input_adapter,
    ts["T"],
    WebsocketAdapterManager,
    typ="T",
    properties=dict,
)

_websocket_output_adapter_def = output_adapter_def(
    "websocket_output_adapter",
    _websocketadapterimpl._websocket_output_adapter,
    WebsocketAdapterManager,
    input=ts["T"],
    properties=dict,
)

_websocket_header_update_adapter_def = output_adapter_def(
    "websocket_header_update_adapter",
    _websocketadapterimpl._websocket_header_update_adapter,
    WebsocketAdapterManager,
    input=ts[List[WebsocketHeaderUpdate]],
)

_websocket_connection_request_adapter_def = output_adapter_def(
    "websocket_connection_request_adapter",
    _websocketadapterimpl._websocket_connection_request_adapter,
    WebsocketAdapterManager,
    input=ts[List[InternalConnectionRequest]],  # needed, List[dict] didn't work on c++ level
    properties=dict,
)
