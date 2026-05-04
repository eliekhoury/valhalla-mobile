import ValhallaObjc
import ValhallaModels
import ValhallaConfigModels

public protocol ValhallaProviding {

    init(_ config: ValhallaConfig) throws

    init(configPath: String) throws

    func route(request: RouteRequest) throws -> RouteResponse
    func route(rawRequest: String) -> String
    func traceRoute(rawRequest: String) -> String
    func traceAttributes(rawRequest: String) -> String
    func locate(rawRequest: String) -> String
    func optimizedRoute(rawRequest: String) -> String
}

public final class Valhalla: ValhallaProviding {
    private let actor: ValhallaWrapper?
    private let configPath: String

    public convenience init(_ config: ValhallaConfig) throws {
        let configURL = try ValhallaFileManager.saveConfigTo(config)
        try self.init(configPath: configURL.relativePath)
    }

    public required init(configPath: String) throws {
        do {
            try ValhallaFileManager.injectTzdataIntoLibrary()
        } catch {
            // If you're circumventing this libraries injection, download tzdata.tar and put in your bundle. https://www.iana.org/time-zones
            fatalError("tzdata was not inject into Bundle.main. This can be avoided by including tzdata.tar in your main bundle.")
        }

        self.configPath = configPath
        do {
            self.actor = try ValhallaWrapper(configPath: configPath)
        } catch let error as NSError {
            throw ValhallaError.valhallaError(error.code, error.domain)
        } catch {
            throw ValhallaError.valhallaError(-1, error.localizedDescription)
        }
    }
    
    public func route(request: RouteRequest) throws -> RouteResponse {
        let requestData = try JSONEncoder().encode(request)
        guard let requestStr = String(data: requestData, encoding: .utf8) else {
            throw ValhallaError.encodingNotUtf8("requestStr")
        }
        
        let resultStr = route(rawRequest: requestStr)
        guard let resultData = resultStr.data(using: .utf8) else {
            throw ValhallaError.encodingNotUtf8("resultData")
        }
        
        if let error = try? JSONDecoder().decode(ValhallaErrorModel.self, from: resultData) {
            throw ValhallaError.valhallaError(error.code, error.message)
        }
        
        return try JSONDecoder().decode(RouteResponse.self, from: resultData)
    }

    public func route(rawRequest request: String) -> String {
        actor!.route(request)
    }

    /// Map matching that returns an OSRM-shape (or Valhalla-shape)
    /// route response. Mirrors `actor->trace_route()` from Valhalla's
    /// `tyr::actor_t`. The request body must include `shape` (the GPS
    /// trace) and a costing.
    public func traceRoute(rawRequest request: String) -> String {
        actor!.traceRoute(request)
    }

    /// Map matching that returns per-edge attributes (road class,
    /// names, durations, surface, …) along the matched path.
    /// Mirrors `actor->trace_attributes()`.
    public func traceAttributes(rawRequest request: String) -> String {
        actor!.traceAttributes(request)
    }

    /// Snap a coordinate to the nearest routable edge(s). Mirrors
    /// `actor->locate()`. Useful for finding the road the rider is
    /// currently on without computing a full route.
    public func locate(rawRequest request: String) -> String {
        actor!.locate(request)
    }

    /// Compute a TSP-style ordering of the supplied locations and
    /// return the optimized multi-waypoint route. Mirrors
    /// `actor->optimized_route()`.
    public func optimizedRoute(rawRequest request: String) -> String {
        actor!.optimizedRoute(request)
    }
}
