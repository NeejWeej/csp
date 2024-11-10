import os
import pytest
import pytz
import threading
from contextlib import contextmanager
from datetime import datetime, timedelta
from typing import List

import csp
from csp import ts

# csp.set_print_full_exception_stack(True)

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
    )

    class EchoWebsocketHandler(tornado.websocket.WebSocketHandler):
        def on_message(self, msg):
            return self.write_message(msg)

    @contextmanager
    def tornado_server(port: int = 8001):
        ready_event2 = threading.Event()
        io_loop2 = None
        app2 = None
        io_thread2 = None

        def run_io_loop2():
            nonlocal io_loop2, app2  # Use nonlocal to modify outer scope variables
            io_loop2 = tornado.ioloop.IOLoop()
            io_loop2.make_current()
            app2 = tornado.web.Application([(r"/", EchoWebsocketHandler)])
            app2.listen(port)
            ready_event2.set()
            io_loop2.start()

        io_thread2 = threading.Thread(target=run_io_loop2)
        io_thread2.start()
        ready_event2.wait()

        try:
            yield
        finally:
            io_loop2.add_callback(io_loop2.stop)
            if io_thread2:
                io_thread2.join(timeout=5)
                if io_thread2.is_alive():
                    raise RuntimeError("IOLoop failed to stop")


@pytest.mark.skipif(not os.environ.get("CSP_TEST_WEBSOCKET"), reason="Skipping websocket adapter tests")
class TestWebsocket:
    @pytest.fixture(scope="class", autouse=True)
    def setup_tornado(self, request):
        # Create class-level attributes
        request.cls.ready_event = threading.Event()

        def run_io_loop():
            request.cls.io_loop = tornado.ioloop.IOLoop()
            request.cls.io_loop.make_current()
            request.cls.app = tornado.web.Application([(r"/", EchoWebsocketHandler)])
            request.cls.app.listen(8000)
            request.cls.ready_event.set()  # Signal that setup is complete
            request.cls.io_loop.start()

        request.cls.io_thread = threading.Thread(target=run_io_loop)
        request.cls.io_thread.start()
        request.cls.ready_event.wait()  # Wait for IOLoop to be ready

        # Teardown
        yield

        request.cls.io_loop.add_callback(request.cls.io_loop.stop)
        if request.cls.io_thread:
            request.cls.io_thread.join(timeout=5)  # Add timeout to prevent hanging
            if request.cls.io_thread.is_alive():
                raise RuntimeError("IOLoop failed to stop")

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

    def test_dynamic_pruned_subscribe_raises(self):
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
            # This gets pruned by csp, which is problematic so we throw in this case.
            recv2 = ws.subscribe(str, RawTextMessageMapper(), connection_request=conn_request)

            csp.add_graph_output("recv", recv)
            csp.stop_engine(recv)

        with pytest.raises(
            ValueError, match=r".*Input adapter has been pruned.*Subscribe call at index: 1 has been pruned"
        ):
            csp.run(g, starttime=datetime.now(pytz.UTC), endtime=timedelta(seconds=1), realtime=True)

    def test_dynamic_multiple_subscribers(self):
        with tornado_server():

            @csp.graph
            def g():
                ws = WebsocketAdapterManager(dynamic=True)
                conn_request1 = csp.const(
                    ConnectionRequest(uri="ws://localhost:8000/", on_connect_payload="hey world from 8000")
                )
                conn_request2 = csp.const(
                    ConnectionRequest(uri="ws://localhost:8001/", on_connect_payload="hey world from 8001")
                )
                recv = ws.subscribe(str, RawTextMessageMapper(), connection_request=conn_request1)
                recv2 = ws.subscribe(str, RawTextMessageMapper(), connection_request=conn_request2)

                csp.add_graph_output("recv", recv)
                csp.add_graph_output("recv2", recv2)

                merged = csp.flatten([recv, recv2])
                stop = csp.filter(csp.count(merged) == 2, merged)
                csp.stop_engine(stop)

            msgs = csp.run(g, starttime=datetime.now(pytz.UTC), endtime=timedelta(seconds=5), realtime=True)
            assert len(msgs["recv"]) == 1
            assert msgs["recv"][0][1].msg == "hey world from 8000"
            assert msgs["recv"][0][1].uri == "ws://localhost:8000/"
            assert len(msgs["recv2"]) == 1
            assert msgs["recv2"][0][1].msg == "hey world from 8001"
            assert msgs["recv2"][0][1].uri == "ws://localhost:8001/"

    def test_unkown_host_graceful_shutdown(self):
        @csp.graph
        def g():
            ws = WebsocketAdapterManager("wss://localhost/")
            assert ws._properties["port"] == "443"
            csp.stop_engine(ws.status())

        csp.run(g, starttime=datetime.now(pytz.UTC), realtime=True)

    def test_send_recv_burst_json(self):
        class MsgStruct(csp.Struct):
            a: int
            b: str

        @csp.node
        def send_msg_on_open(status: ts[Status]) -> ts[str]:
            if csp.ticked(status):
                return MsgStruct(a=1234, b="im a string").to_json()

        @csp.node
        def my_edge_that_handles_burst(objs: ts[List[MsgStruct]]) -> ts[bool]:
            if csp.ticked(objs):
                return True

        @csp.graph
        def g():
            ws = WebsocketAdapterManager("ws://localhost:8000/")
            status = ws.status()
            ws.send(send_msg_on_open(status))
            recv = ws.subscribe(MsgStruct, JSONTextMessageMapper(), push_mode=csp.PushMode.BURST)
            _ = my_edge_that_handles_burst(recv)
            csp.add_graph_output("recv", recv)
            csp.stop_engine(recv)

        msgs = csp.run(g, starttime=datetime.now(pytz.UTC), realtime=True)
        obj = msgs["recv"][0][1]
        assert isinstance(obj, list)
        innerObj = obj[0]
        assert innerObj.a == 1234
        assert innerObj.b == "im a string"
