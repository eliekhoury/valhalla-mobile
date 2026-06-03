#include <boost/property_tree/ptree.hpp>
#include <valhalla/tyr/actor.h>
#include <valhalla/baldr/rapidjson_utils.h>
#include <valhalla/loki/worker.h>
#include "valhalla_actor.h"

// walk_forward: drives `graph_reader` directly (forward-star topology walk).
#include <valhalla/baldr/graphreader.h>
#include <valhalla/baldr/graphtile.h>
#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/directededge.h>
#include <valhalla/baldr/nodeinfo.h>
#include <valhalla/baldr/edgeinfo.h>
#include <valhalla/baldr/graphconstants.h>
#include <valhalla/baldr/location.h>
#include <valhalla/baldr/pathlocation.h>
#include <valhalla/loki/search.h>
#include <valhalla/sif/costfactory.h>
#include <valhalla/sif/dynamiccost.h>
#include <valhalla/midgard/pointll.h>
#include <valhalla/midgard/encoded.h>
#include <valhalla/worker.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <unordered_set>
#include <algorithm>

class TileGetterWrapper : public valhalla::baldr::tile_getter_t {
public:
  /**
   * @param pool_size  the number of curler instances in the pool
   * @param user_agent  user agent to use by curlers for HTTP requests
   * @param gzipped  whether to request for gzip compressed data
   * @param user_pw  the "user:pwd" for HTTP basic auth
   */
  TileGetterWrapper(ValhallaMobileHttpClient* http_client, bool is_gzipped): http_client(http_client), is_gzipped(is_gzipped) {
  }

  GET_response_t get(const std::string& url,
                     const uint64_t range_offset = 0,
                     const uint64_t range_size = 0) override {
    GET_response_t result;
    if (http_client) { 
        result = http_client->get(url, range_offset, range_size);
    } else {
        result.status_ = tile_getter_t::status_code_t::FAILURE;
    }
    return result;
  }

  HEAD_response_t head(const std::string& url, header_mask_t header_mask) override {
    HEAD_response_t result;
    if (http_client) { 
        result = http_client->head(url, header_mask);
    } else {
        result.status_ = tile_getter_t::status_code_t::FAILURE;
    }
    return result;
  }

  bool gzipped() const override {
    return is_gzipped;
  }

  ~TileGetterWrapper() {
    delete http_client;
  };

private:
  bool is_gzipped;
  ValhallaMobileHttpClient* http_client;
};


ValhallaActor::ValhallaActor(const std::string& config_path, ValhallaMobileHttpClient* http_client) {
std::string config_file(config_path);
    
    // Set up the config object
    boost::property_tree::ptree config;
    rapidjson::read_json(config_file, config);

    auto mjolnir_config = config.get_child("mjolnir");
    graph_reader = std::make_unique<valhalla::baldr::GraphReader>(
      mjolnir_config, 
      std::make_unique<TileGetterWrapper>(http_client, mjolnir_config.get<bool>("tile_url_gz", false))
    );
    // Setup the actor
    actor = std::make_unique<valhalla::tyr::actor_t>(config, *graph_reader, true);
}

std::string ValhallaActor::route(const std::string& request) {
    return actor->route(std::string(request));
}

std::string ValhallaActor::trace_route(const std::string& request) {
    return actor->trace_route(std::string(request));
}

std::string ValhallaActor::trace_attributes(const std::string& request) {
    return actor->trace_attributes(std::string(request));
}

std::string ValhallaActor::locate(const std::string& request) {
    return actor->locate(std::string(request));
}

// Graph-walk a deterministic forward corridor from (lat,lon) along `bearing`.
// NOT a route: loki seeds the heading-resolved start directed edge, then we
// forward-star edge→endnode→best same-way/straightest continuation up to
// `max_distance`, emitting per-edge way_id / names / road_class / mean_elevation
// and a polyline6 shape. Custom fork action — no upstream actor-> equivalent.
std::string ValhallaActor::walk_forward(const std::string& request) {
    using namespace valhalla;
    using baldr::GraphId; using baldr::DirectedEdge; using baldr::NodeInfo;
    using baldr::EdgeInfo; using baldr::graph_tile_ptr; using midgard::PointLL;

    rapidjson::Document doc; doc.Parse(request.c_str());
    if (doc.HasParseError() || !doc.HasMember("lat") || !doc.HasMember("lon"))
        return R"({"edges":[],"error":"walk_forward: missing lat/lon"})";
    const double lat = doc["lat"].GetDouble();
    const double lon = doc["lon"].GetDouble();
    const float  bearing      = doc.HasMember("bearing") ? doc["bearing"].GetFloat() : 0.f;
    const double max_distance = doc.HasMember("max_distance") ? doc["max_distance"].GetDouble() : 3000.0;
    const float  heading_tol  = doc.HasMember("heading_tolerance") ? doc["heading_tolerance"].GetFloat() : 45.f;
    const double radius       = doc.HasMember("radius") ? doc["radius"].GetDouble() : 30.0;
    const bool   dbg          = doc.HasMember("debug") && doc["debug"].GetBool();
    if (dbg) fprintf(stderr, "[WF] enter lat=%.5f lon=%.5f bearing=%.0f maxd=%.0f\n",
                     lat, lon, bearing, max_distance);

    // 1. costing (drives Allowed() + loki heading/access filtering)
    Api api;
    ParseApi(request, Options::route, api);
    sif::CostFactory factory;
    sif::cost_ptr_t costing = factory.Create(api.options());

    // 2. resolve start directed edge via loki (heading picks travel direction)
    baldr::Location loc(PointLL(lon, lat));            // PointLL is (lng, lat)
    loc.heading_ = bearing;
    loc.heading_tolerance_ = heading_tol;
    loc.min_outbound_reach_ = 1;
    loc.min_inbound_reach_  = 0;
    loc.radius_ = radius;
    loki::Search searcher(*graph_reader);
    auto results = searcher.search({loc}, costing);
    auto it = results.find(loc);
    if (it == results.end() || it->second.edges.empty()) {
        if (dbg) fprintf(stderr, "[WF] no start edge (loki search empty)\n");
        return R"({"edges":[],"error":"no start edge"})";
    }
    const auto& seed = it->second.edges.front();
    GraphId edge_id = seed.id;
    const double start_pct = seed.percent_along;

    // 3. forward walk
    rapidjson::Document out(rapidjson::kObjectType);
    auto& al = out.GetAllocator();
    rapidjson::Value edges(rapidjson::kArrayType);

    graph_tile_ptr tile = nullptr;
    std::unordered_set<GraphId> visited;
    double traveled = 0; bool first = true;
    double inbound_heading = bearing;

    while (traveled < max_distance && edge_id.is_valid() && !visited.count(edge_id)) {
        visited.insert(edge_id);
        const DirectedEdge* de = graph_reader->directededge(edge_id, tile);
        if (!de) break;                                // null tile = end of reachable graph
        EdgeInfo info = tile->edgeinfo(de);

        // shape: copy out before tile may swap; honor forward() orientation
        std::vector<PointLL> shp = info.shape();
        if (!de->forward()) std::reverse(shp.begin(), shp.end());
        // Heading ARRIVING at the endnode, in travel direction, from the FULL shape
        // (before the start is trimmed) — drives the straightness of the next choice.
        const double arrive_heading = (shp.size() >= 2)
            ? PointLL::HeadingAtEndOfPolyline(shp, 20.0) : inbound_heading;
        if (first && start_pct > 0.0 && shp.size() >= 2) {
            size_t drop = static_cast<size_t>(start_pct * (shp.size() - 1));
            if (drop > 0 && drop < shp.size()) shp.erase(shp.begin(), shp.begin() + drop);
        } else if (!first && !shp.empty()) {
            shp.erase(shp.begin());                    // dedup shared junction vertex
        }

        rapidjson::Value e(rapidjson::kObjectType);
        e.AddMember("way_id", (uint64_t)info.wayid(), al);
        e.AddMember("length_m", (uint32_t)de->length(), al);
        e.AddMember("road_class", (int)de->classification(), al);
        e.AddMember("use", (int)de->use(), al);
        if (info.has_elevation())
            e.AddMember("mean_elevation", info.mean_elevation(), al);
        rapidjson::Value names(rapidjson::kArrayType);
        for (const auto& n : info.GetNames()) {
            rapidjson::Value s; s.SetString(n.c_str(), (rapidjson::SizeType)n.size(), al);
            names.PushBack(s, al);
        }
        e.AddMember("names", names, al);
        std::string enc = midgard::encode<std::vector<PointLL>>(shp, 1e6);
        rapidjson::Value shape; shape.SetString(enc.c_str(), (rapidjson::SizeType)enc.size(), al);
        e.AddMember("shape", shape, al);
        edges.PushBack(e, al);

        traveled += de->length() * (first ? (1.0 - start_pct) : 1.0);
        first = false;
        inbound_heading = arrive_heading;

        // advance: endnode -> forward-star -> best continuation
        GraphId node_id = de->endnode();
        graph_tile_ptr ntile = tile;
        const NodeInfo* node = graph_reader->nodeinfo(node_id, ntile);
        if (!node) break;
        const GraphId opp = graph_reader->GetOpposingEdgeId(edge_id);
        const uint32_t first_e = node->edge_index();
        const uint32_t cnt = node->edge_count();

        // Pick the best continuation: straightest (travel-direction headings), with a
        // same-way bonus (the road continues as the same OSM way), a road-class
        // preference (stay on a comparable road; penalize dropping onto a service
        // road / driveway / path off a real road), and a small bike-friendly nudge.
        GraphId best; double best_score = -1e9;
        const uint64_t cur_way = info.wayid();
        const int cur_class = static_cast<int>(de->classification());
        if (dbg) {
            const std::string cn = info.GetNames().empty() ? std::string("?") : info.GetNames()[0];
            fprintf(stderr, "[WF] node after \"%s\" way=%llu cls=%d arrive_hdg=%.0f  (%u edges)\n",
                   cn.c_str(), (unsigned long long)cur_way, cur_class, inbound_heading, cnt);
        }
        for (uint32_t i = 0; i < cnt; ++i) {
            GraphId cid(node_id.tileid(), node_id.level(), first_e + i);
            const DirectedEdge* c = ntile->directededge(first_e + i);
            EdgeInfo cinfo = ntile->edgeinfo(c);
            const std::string cn = cinfo.GetNames().empty() ? std::string("?") : cinfo.GetNames()[0];
            const int cc = static_cast<int>(c->classification());

            const bool is_sc = c->is_shortcut();                 // collapsed through-chain (arterial/highway)
            const char* skip = nullptr;
            if (cid == opp) skip = "uturn";                      // no U-turn
            else if (c->IsTransitLine() || c->use() == baldr::Use::kTransitConnection) skip = "transit";
            else if (!is_sc && !costing->Allowed(c, ntile)) skip = "not-allowed";  // shortcuts recovered to local below
            if (skip) {
                if (dbg) fprintf(stderr, "[WF]    skip \"%s\" way=%llu cls=%d (%s)\n",
                                cn.c_str(), (unsigned long long)cinfo.wayid(), cc, skip);
                continue;
            }

            // Candidate heading LEAVING the node, in travel direction (reverse the
            // edgeinfo shape when the directed edge runs against it).
            std::vector<PointLL> cshape = cinfo.shape();
            if (!c->forward()) std::reverse(cshape.begin(), cshape.end());
            const double ch = (cshape.size() >= 2)
                ? PointLL::HeadingAlongPolyline(cshape, 20.0) : inbound_heading;

            const double diff = std::abs(ch - inbound_heading);
            const double turn = std::min(diff, 360.0 - diff);    // 0..180 deviation from straight
            double score = 180.0 - turn;                         // straighter = higher
            if (cinfo.wayid() == cur_way) score += 120.0;        // same OSM way continues the road
            if (cc == cur_class) score += 40.0;
            else if (cc > cur_class) score -= double(cc - cur_class) * 20.0;  // bigger ordinal = smaller road
            if (c->use() == baldr::Use::kCycleway || c->bike_network()) score += 30.0;

            if (dbg) fprintf(stderr, "[WF]    cand \"%s\" way=%llu cls=%d hdg=%.0f turn=%.0f score=%.0f\n",
                            cn.c_str(), (unsigned long long)cinfo.wayid(), cc, ch, turn, score);
            if (score > best_score) { best_score = score; best = cid; }
        }
        if (!best.is_valid()) { if (dbg) fprintf(stderr, "[WF] -> dead end\n"); break; }   // dead end
        // If the chosen continuation is a shortcut (a forced through-chain collapsed on
        // the arterial/highway hierarchy, way_id=0), recover its constituent LOCAL edges
        // and resume there: the local level carries full detail and has no shortcuts, so
        // the rest of the corridor walks cleanly. Fixes the shortcut dead-end.
        {
            const DirectedEdge* bde = ntile->directededge(best);
            if (bde && bde->is_shortcut()) {
                std::vector<GraphId> recovered = graph_reader->RecoverShortcut(best);
                if (!recovered.empty() && recovered.front().is_valid()) {
                    if (dbg) fprintf(stderr, "[WF]    recovered shortcut -> %zu local edges\n", recovered.size());
                    best = recovered.front();
                }
            }
        }
        edge_id = best;
        tile = ntile;
    }

    out.AddMember("edges", edges, al);
    out.AddMember("traveled_m", traveled, al);
    rapidjson::StringBuffer sb; rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    out.Accept(w);
    return sb.GetString();
}

std::string ValhallaActor::optimized_route(const std::string& request) {
    return actor->optimized_route(std::string(request));
}
