import os
import pytest
import pytz
import threading
from contextlib import contextmanager
from datetime import datetime, timedelta
from typing import List

import csp
from csp import ts

if os.environ.get("CSP_TEST_WEBSOCKET"):
    import tornado.ioloop
    import tornado.web
    import tornado.websocket

    from csp.adapters.websocket import (
        ActionType,
        ConnectionRequest,
        JSONTextMessageMapper,
        RawTextMessageMapper,
        Status,
        WebsocketAdapterManager,
        WebsocketStatus,
    )

    class EchoWebsocketHandler(tornado.websocket.WebSocketHandler):
        def on_message(self, msg):
            return self.write_message(msg)

    @contextmanager
    def create_tornado_server(port: int):
        """Base context manager for creating a Tornado server in a thread"""
        ready_event = threading.Event()
        io_loop = None
        app = None
        io_thread = None

        def run_io_loop():
            nonlocal io_loop, app
            io_loop = tornado.ioloop.IOLoop()
            io_loop.make_current()
            app = tornado.web.Application([(r"/", EchoWebsocketHandler)])
            app.listen(port)
            ready_event.set()
            io_loop.start()

        io_thread = threading.Thread(target=run_io_loop)
        io_thread.start()
        ready_event.wait()

        try:
            yield io_loop, app, io_thread
        finally:
            io_loop.add_callback(io_loop.stop)
            if io_thread:
                io_thread.join(timeout=5)
                if io_thread.is_alive():
                    raise RuntimeError("IOLoop failed to stop")

    @contextmanager
    def tornado_server(port: int = 8001):
        """Simplified context manager that uses the base implementation"""
        with create_tornado_server(port) as (_io_loop, _app, _io_thread):
            yield


@pytest.mark.skipif(os.environ.get("CSP_TEST_WEBSOCKET") is None, reason="'CSP_TEST_WEBSOCKET' env variable is not set")
class TestWebsocket:
    @pytest.fixture(scope="class", autouse=True)
    def setup_tornado(self, request):
        with create_tornado_server(8000) as (io_loop, app, io_thread):
            request.cls.io_loop = io_loop
            request.cls.app = app
            request.cls.io_thread = io_thread
            yield

    def test_send_recv_msg(self):
        @csp.node
        def send_msg_on_open(status: ts[Status]) -> ts[str]:
            if csp.ticked(status):
                return "Hello, World!"

        @csp.graph
        def g():
            ws = WebsocketAdapterManager("ws://localhost:8000/")
            status = ws.status()
            ws.send(send_msg_on_open(status))
            recv = ws.subscribe(str, RawTextMessageMapper())

            csp.add_graph_output("recv", recv)
            csp.stop_engine(recv)

        msgs = csp.run(g, starttime=datetime.now(pytz.UTC), realtime=True)
        assert msgs["recv"][0][1] == "Hello, World!"

    @pytest.mark.parametrize("send_payload_subscribe", [True, False])
    def test_send_recv_json_dynamic_on_connect_payload(self, send_payload_subscribe):
        class MsgStruct(csp.Struct):
            a: int
            b: str

        @csp.graph
        def g():
            ws = WebsocketAdapterManager(dynamic=True)
            conn_request = ConnectionRequest(
                uri="ws://localhost:8000/",
                action=ActionType.CONNECT,
                on_connect_payload=MsgStruct(a=1234, b="im a string").to_json(),
            )
            if not send_payload_subscribe:
                # We send payload via the dummy send function
                # The 'on_connect_payload sends the result
                ws.send(csp.null_ts(object), connection_request=csp.const(conn_request))
            subscribe_connection_request = (
                ConnectionRequest(uri="ws://localhost:8000/", action=ActionType.CONNECT)
                if not send_payload_subscribe
                else conn_request
            )
            recv = ws.subscribe(
                MsgStruct, JSONTextMessageMapper(), connection_request=csp.const(subscribe_connection_request)
            )

            csp.add_graph_output("recv", recv)
            csp.stop_engine(recv)

        msgs = csp.run(g, starttime=datetime.now(pytz.UTC), realtime=True)
        obj = msgs["recv"][0][1]
        assert obj.uri == "ws://localhost:8000/"
        true_obj = obj.msg
        assert isinstance(true_obj, MsgStruct)
        assert true_obj.a == 1234
        assert true_obj.b == "im a string"

    def test_send_recv_json(self):
        class MsgStruct(csp.Struct):
            a: int
            b: str

        @csp.node
        def send_msg_on_open(status: ts[Status]) -> ts[str]:
            if csp.ticked(status):
                return MsgStruct(a=1234, b="im a string").to_json()

        @csp.graph
        def g():
            ws = WebsocketAdapterManager("ws://localhost:8000/")
            status = ws.status()
            ws.send(send_msg_on_open(status))
            recv = ws.subscribe(MsgStruct, JSONTextMessageMapper())

            csp.add_graph_output("recv", recv)
            csp.stop_engine(recv)

        msgs = csp.run(g, starttime=datetime.now(pytz.UTC), realtime=True)
        obj = msgs["recv"][0][1]
        assert isinstance(obj, MsgStruct)
        assert obj.a == 1234
        assert obj.b == "im a string"

    def test_send_multiple_and_recv_msgs(self):
        @csp.node
        def send_msg_on_open(status: ts[Status], idx: int) -> ts[str]:
            if csp.ticked(status):
                return f"Hello, World! {idx}"

        @csp.node
        def stop_on_all_or_timeout(msgs: ts[str], l: int = 50) -> ts[bool]:
            with csp.alarms():
                a_timeout: ts[bool] = csp.alarm(bool)

            with csp.state():
                s_ctr = 0

            with csp.start():
                csp.schedule_alarm(a_timeout, timedelta(seconds=5), False)

            if csp.ticked(msgs):
                s_ctr += 1

            if csp.ticked(a_timeout) or (csp.ticked(msgs) and s_ctr == l):
                return True

        @csp.graph
        def g(n: int):
            ws = WebsocketAdapterManager("ws://localhost:8000/")
            status = ws.status()
            ws.send(csp.flatten([send_msg_on_open(status, i) for i in range(n)]))
            recv = ws.subscribe(str, RawTextMessageMapper())

            csp.add_graph_output("recv", recv)
            csp.stop_engine(stop_on_all_or_timeout(recv, n))

        n = 100
        msgs = csp.run(g, n, starttime=datetime.now(pytz.UTC), realtime=True)
        assert len(msgs["recv"]) == n
        assert msgs["recv"][0][1] != msgs["recv"][-1][1]

    def test_send_multiple_and_recv_msgs_dynamic(self):
        @csp.graph
        def g():
            ws = WebsocketAdapterManager(dynamic=True)
            conn_request = csp.const(
                ConnectionRequest(
                    uri="ws://localhost:8000/",
                    action=ActionType.CONNECT,
                )
            )
            val = csp.curve(int, [(timedelta(milliseconds=50), 0), (timedelta(milliseconds=500), 1)])
            hello = csp.apply(val, lambda x: f"hi world{x}", str)
            delayed_conn_req = csp.delay(conn_request, delay=timedelta(milliseconds=100))

            # We connect immediately and send out the hello message
            ws.send(hello, connection_request=conn_request)

            recv = ws.subscribe(str, RawTextMessageMapper(), connection_request=delayed_conn_req)
            # This call connects first
            recv2 = ws.subscribe(str, RawTextMessageMapper(), connection_request=conn_request)

            merged = csp.flatten([recv, recv2])
            csp.add_graph_output("recv", merged.msg)

            stop = csp.filter(csp.count(merged) == 3, merged)
            csp.stop_engine(stop)

        msgs = csp.run(g, starttime=datetime.now(pytz.UTC), endtime=timedelta(seconds=1), realtime=True)
        assert len(msgs["recv"]) == 3
        # the first message sent out, only the second subscribe call picks this up
        assert msgs["recv"][0][1] == "hi world0"
        # Both the subscribe calls receive this message
        assert msgs["recv"][1][1] == "hi world1"
        assert msgs["recv"][2][1] == "hi world1"

    def test_dynamic_disconnect_connect_pruned_subscribe(self):
        @csp.graph
        def g():
            ws = WebsocketAdapterManager(dynamic=True)

            conn_request = csp.curve(
                ConnectionRequest,
                [
                    (timedelta(), ConnectionRequest(uri="ws://localhost:8000/")),
                    (
                        timedelta(milliseconds=100),
                        ConnectionRequest(uri="ws://localhost:8000/", action=ActionType.DISCONNECT),
                    ),
                    (timedelta(milliseconds=350), ConnectionRequest(uri="ws://localhost:8000/")),
                ],
            )
            const_conn_request = csp.const(ConnectionRequest(uri="ws://localhost:8000/"))
            val = csp.curve(int, [(timedelta(milliseconds=200), 0), (timedelta(milliseconds=500), 1)])
            hello = csp.apply(val, lambda x: f"hi world{x}", str)

            # We connect immediately and send out the hello message
            ws.send(hello, connection_request=const_conn_request)

            recv = ws.subscribe(str, RawTextMessageMapper(), connection_request=conn_request)
            # This gets pruned by csp
            recv2 = ws.subscribe(str, RawTextMessageMapper(), connection_request=conn_request)
            recv3 = ws.subscribe(str, RawTextMessageMapper(), connection_request=const_conn_request)

            csp.add_graph_output("recv", recv)
            csp.add_graph_output("recv3", recv3)
            csp.stop_engine(recv)

        msgs = csp.run(g, starttime=datetime.now(pytz.UTC), endtime=timedelta(seconds=1), realtime=True)
        assert len(msgs["recv"]) == 1
        assert len(msgs["recv3"]) == 2
        # Only the second message is received, since we disonnect before the first one is sent
        assert msgs["recv"][0][1].msg == "hi world1"
        assert msgs["recv"][0][1].uri == "ws://localhost:8000/"

        # This subscribe call received all the messages
        assert msgs["recv3"][0][1].msg == "hi world0"
        assert msgs["recv3"][0][1].uri == "ws://localhost:8000/"
        assert msgs["recv3"][1][1].msg == "hi world1"
        assert msgs["recv3"][1][1].uri == "ws://localhost:8000/"

    def test_dynamic_pruned_subscribe(self):
        @csp.graph
        def g():
            ws = WebsocketAdapterManager(dynamic=True)
            conn_request = csp.const(
                ConnectionRequest(
                    uri="ws://localhost:8000/",
                    action=ActionType.CONNECT,
                )
            )
            val = csp.curve(int, [(timedelta(milliseconds=50), 0), (timedelta(milliseconds=500), 1)])
            hello = csp.apply(val, lambda x: f"hi world{x}", str)
            delayed_conn_req = csp.delay(conn_request, delay=timedelta(milliseconds=100))

            # We connect immediately and send out the hello message
            ws.send(hello, connection_request=conn_request)

            recv = ws.subscribe(str, RawTextMessageMapper(), connection_request=delayed_conn_req)
            # This gets pruned by csp
            recv2 = ws.subscribe(str, RawTextMessageMapper(), connection_request=conn_request)

            csp.add_graph_output("recv", recv)
            csp.stop_engine(recv)

        msgs = csp.run(g, starttime=datetime.now(pytz.UTC), endtime=timedelta(seconds=2), realtime=True)
        assert len(msgs["recv"]) == 1
        # Only the second message is received
        assert msgs["recv"][0][1].msg == "hi world1"
        assert msgs["recv"][0][1].uri == "ws://localhost:8000/"

    def test_dynamic_multiple_subscribers(self):
        @csp.node
        def send_on_status(status: ts[Status], uri: str, val: str) -> ts[str]:
            if csp.ticked(status):
                if uri in status.msg and status.status_code == WebsocketStatus.ACTIVE.value:
                    return val

        with tornado_server():
            # We do this to only spawn the tornado server once for both options
            @csp.graph
            def g(use_on_connect_payload: bool):
                ws = WebsocketAdapterManager(dynamic=True)
                if use_on_connect_payload:
                    conn_request1 = csp.const(
                        ConnectionRequest(uri="ws://localhost:8000/", on_connect_payload="hey world from 8000")
                    )
                    conn_request2 = csp.const(
                        ConnectionRequest(uri="ws://localhost:8001/", on_connect_payload="hey world from 8001")
                    )
                else:
                    conn_request1 = csp.const(ConnectionRequest(uri="ws://localhost:8000/"))
                    conn_request2 = csp.const(ConnectionRequest(uri="ws://localhost:8001/"))
                    status = ws.status()
                    to_send = send_on_status(status, "ws://localhost:8000/", "hey world from 8000")
                    to_send2 = send_on_status(status, "ws://localhost:8001/", "hey world from 8001")
                    ws.send(to_send, connection_request=conn_request1)
                    ws.send(to_send2, connection_request=conn_request2)

                recv = ws.subscribe(str, RawTextMessageMapper(), connection_request=conn_request1)
                recv2 = ws.subscribe(str, RawTextMessageMapper(), connection_request=conn_request2)

                csp.add_graph_output("recv", recv)
                csp.add_graph_output("recv2", recv2)

                merged = csp.flatten([recv, recv2])
                stop = csp.filter(csp.count(merged) == 2, merged)
                csp.stop_engine(stop)

            for use_on_connect_payload in [True, False]:
                msgs = csp.run(
                    g,
                    use_on_connect_payload,
                    starttime=datetime.now(pytz.UTC),
                    endtime=timedelta(seconds=5),
                    realtime=True,
                )
                assert len(msgs["recv"]) == 1
                assert msgs["recv"][0][1].msg == "hey world from 8000"
                assert msgs["recv"][0][1].uri == "ws://localhost:8000/"
                assert len(msgs["recv2"]) == 1
                assert msgs["recv2"][0][1].msg == "hey world from 8001"
                assert msgs["recv2"][0][1].uri == "ws://localhost:8001/"

    @pytest.mark.parametrize("dynamic", [False, True])
    def test_send_recv_burst_json(self, dynamic):
        class MsgStruct(csp.Struct):
            a: int
            b: str

        @csp.node
        def my_edge_that_handles_burst(objs: ts[List[MsgStruct]]):
            if csp.ticked(objs):
                # Does nothing but makes sure it's not pruned
                ...

        @csp.graph
        def g():
            if dynamic:
                ws = WebsocketAdapterManager(dynamic=True)
                wrapped_recv = ws.subscribe(
                    MsgStruct,
                    JSONTextMessageMapper(),
                    push_mode=csp.PushMode.BURST,
                    connection_request=csp.const(
                        ConnectionRequest(
                            uri="ws://localhost:8000/", on_connect_payload=MsgStruct(a=1234, b="im a string").to_json()
                        )
                    ),
                )
                recv = csp.apply(wrapped_recv, lambda vals: [v.msg for v in vals], List[MsgStruct])
            else:
                ws = WebsocketAdapterManager("ws://localhost:8000/")
                status = ws.status()
                ws.send(csp.apply(status, lambda _x: MsgStruct(a=1234, b="im a string").to_json(), str))
                recv = ws.subscribe(MsgStruct, JSONTextMessageMapper(), push_mode=csp.PushMode.BURST)

            my_edge_that_handles_burst(recv)
            csp.add_graph_output("recv", recv)
            csp.stop_engine(recv)

        msgs = csp.run(g, starttime=datetime.now(pytz.UTC), realtime=True)
        obj = msgs["recv"][0][1]
        assert isinstance(obj, list)
        innerObj = obj[0]
        assert innerObj.a == 1234
        assert innerObj.b == "im a string"

    def test_unkown_host_graceful_shutdown(self):
        @csp.graph
        def g():
            ws = WebsocketAdapterManager("wss://localhost/")
            # We need this since without any input or output
            # adapters, the websocket connection is not actually made.
            ws.send(csp.null_ts(str))
            assert ws._properties["port"] == "443"
            csp.stop_engine(ws.status())

        csp.run(g, starttime=datetime.now(pytz.UTC), realtime=True)

    def test_unkown_host_graceful_shutdown_slow(self):
        @csp.graph
        def g():
            ws = WebsocketAdapterManager("wss://localhost/")
            # We need this since without any input or output
            # adapters, the websocket connection is not actually made.
            ws.send(csp.null_ts(str))
            assert ws._properties["port"] == "443"
            stop_flag = csp.filter(csp.count(ws.status()) == 2, ws.status())
            csp.stop_engine(stop_flag)

        csp.run(g, starttime=datetime.now(pytz.UTC), realtime=True)
