module io.github.sendablemetatype.webrtc.examples {

    requires com.fasterxml.jackson.databind;
    requires java.logging;
    requires java.net.http;
    requires org.eclipse.jetty.server;
    requires org.eclipse.jetty.websocket.server;
    requires io.github.sendablemetatype.webrtc;

    exports io.github.sendablemetatype.webrtc.examples.web.client;
    exports io.github.sendablemetatype.webrtc.examples.web.server;
    exports io.github.sendablemetatype.webrtc.examples.web.model;

}