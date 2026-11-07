"""Opens the host port on the home router with UPnP, so the 3DS can connect from outside.

Most home routers accept UPnP port mappings from devices on their network. The mapping
forwards UDP 5007 to this PC, lasts an hour and is renewed while the host runs; it is
removed when the host closes. Only standard library modules are used.
"""
import re
import socket
import threading
import urllib.error
import urllib.request
from urllib.parse import urljoin, urlparse

SSDP_ADDRESS = ("239.255.255.250", 1900)
SEARCH = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
          "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n")
SERVICES = ("WANIPConnection:1", "WANIPConnection:2", "WANPPPConnection:1")
LEASE_S = 3600
RENEW_S = 1200


class UpnpError(Exception):
    pass


def _find_gateway(timeout=3.0):
    """Returns the URL of the router's UPnP description."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(SEARCH.encode(), SSDP_ADDRESS)
        while True:
            data, _ = sock.recvfrom(4096)
            match = re.search(rb"(?im)^location:\s*(\S+)", data)
            if match:
                return match.group(1).decode()
    except socket.timeout:
        raise UpnpError("no UPnP router answered (UPnP may be disabled on the router)")
    finally:
        sock.close()


def _control_url(location):
    xml = urllib.request.urlopen(location, timeout=3).read().decode(errors="replace")
    for service in SERVICES:
        match = re.search(r"<serviceType>urn:schemas-upnp-org:service:%s</serviceType>.*?"
                          r"<controlURL>([^<]+)</controlURL>" % re.escape(service), xml, re.S)
        if match:
            return urljoin(location, match.group(1)), service
    raise UpnpError("the router does not offer port mapping")


def _soap(url, service, action, arguments=""):
    body = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
            f'<u:{action} xmlns:u="urn:schemas-upnp-org:service:{service}">{arguments}</u:{action}>'
            '</s:Body></s:Envelope>')
    request = urllib.request.Request(url, data=body.encode(), headers={
        "Content-Type": 'text/xml; charset="utf-8"',
        "SOAPAction": f'"urn:schemas-upnp-org:service:{service}#{action}"'})
    try:
        return urllib.request.urlopen(request, timeout=4).read().decode(errors="replace")
    except urllib.error.HTTPError as error:
        detail = re.search(r"<errorDescription>([^<]*)</errorDescription>", error.read().decode(errors="replace"))
        raise UpnpError(f"{action} refused: {detail.group(1) if detail else error.code}")


def _local_ip_towards(host):
    """The address of this PC on the interface that reaches `host`."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect((host, 9))  # no packet is sent for a UDP connect
        return sock.getsockname()[0]
    finally:
        sock.close()


class PortMapper:
    """Keeps UDP `port` forwarded to this PC while the host runs."""

    def __init__(self, port, report=print):
        self.port = port
        self.report = report
        self.state = "off"      # off, searching, open, failed
        self.public = None      # (ip, port) when open
        self.error = None
        self._stop = threading.Event()
        self._url = None
        self._service = None
        self._thread = None

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._url and self.state == "open":
            try:
                _soap(self._url, self._service, "DeletePortMapping",
                      f"<NewRemoteHost></NewRemoteHost><NewExternalPort>{self.port}</NewExternalPort>"
                      "<NewProtocol>UDP</NewProtocol>")
                self.report(f"Router port {self.port} closed")
            except (UpnpError, OSError):
                pass
        self.state, self.public = "off", None

    def _map(self):
        location = _find_gateway()
        self._url, self._service = _control_url(location)
        local_ip = _local_ip_towards(urlparse(location).hostname)
        _soap(self._url, self._service, "AddPortMapping",
              f"<NewRemoteHost></NewRemoteHost><NewExternalPort>{self.port}</NewExternalPort>"
              f"<NewProtocol>UDP</NewProtocol><NewInternalPort>{self.port}</NewInternalPort>"
              f"<NewInternalClient>{local_ip}</NewInternalClient><NewEnabled>1</NewEnabled>"
              f"<NewPortMappingDescription>OldCast</NewPortMappingDescription>"
              f"<NewLeaseDuration>{LEASE_S}</NewLeaseDuration>")
        reply = _soap(self._url, self._service, "GetExternalIPAddress")
        match = re.search(r"<NewExternalIPAddress>([^<]*)</NewExternalIPAddress>", reply)
        if not match or not match.group(1):
            raise UpnpError("the router did not report its public address")
        return match.group(1)

    def _run(self):
        while not self._stop.is_set():
            self.state = "searching" if self.state != "open" else "open"
            try:
                public_ip = self._map()
                if self.public != (public_ip, self.port):
                    self.report(f"Router port {self.port} open: reachable from outside at {public_ip}")
                self.public, self.state, self.error = (public_ip, self.port), "open", None
                wait = RENEW_S
            except (UpnpError, OSError) as error:
                if self.state != "failed" or str(error) != self.error:
                    self.report(f"Could not open the router port: {error}")
                self.public, self.state, self.error = None, "failed", str(error)
                wait = 60
            self._stop.wait(wait)
