module io.github.sendablemetatype.webrtc.examples {

    requires tools.jackson.databind;
    requires java.desktop;
    requires java.logging;
    requires java.net.http;
    requires org.eclipse.jetty.server;
    requires org.eclipse.jetty.websocket.server;
    requires io.github.sendablemetatype.webrtc;
    requires io.github.sendablemetatype.webrtc.media;

    exports io.github.sendablemetatype.webrtc.examples.web.client;
    exports io.github.sendablemetatype.webrtc.examples.web.server;
    exports io.github.sendablemetatype.webrtc.examples.web.model;

}