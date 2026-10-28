#include "mine_teleop/detail/ice_route_policy.hpp"
#include "mine_teleop/detail/ice_diagnostics.hpp"
#include <iostream>
#include <stdexcept>
using namespace mine_teleop::detail;
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int main() {
  try {
    IceRoutePolicy p(3000,true,false); p.begin_attempt();
    require(p.mode()=="direct", "direct stage missing");
    require(!p.allow_relay_retry(90000,false,false,true), "missing answer misclassified as NAT failure");
    require(!p.allow_relay_retry(90000,true,false,false), "budget did not start at applied answer");
    require(!p.allow_relay_retry(92999,true,false,false), "premature fallback");
    require(p.allow_relay_retry(93000,true,false,false)&&p.mode()=="all", "direct deadline did not admit TURN");
    p.begin_attempt();
    require(!p.allow_relay_retry(100000,true,false,true)&&p.fallbacks()==1, "unbounded restart loop");
    IceRoutePolicy connected(3000,true,false); connected.begin_attempt();
    require(!connected.allow_relay_retry(0,true,true,false), "healthy path rebuilt");
    require(!connected.allow_relay_retry(90000,true,false,false), "transient disconnect rebuilt healthy epoch");
    require(connected.allow_relay_retry(90001,true,false,true), "terminal direct failure not retried");
    for (auto disabled : {IceRoutePolicy(0,true,false),IceRoutePolicy(3000,false,false),IceRoutePolicy(3000,true,true)})
      require(!disabled.allow_relay_retry(30000,true,false,true), "disabled/forced-relay policy changed");
    for (const auto& [ip,scope] : {std::pair{"10.0.0.1","private"},{"100.64.0.2","shared_cgnat"},{"8.8.8.8","public"},
        {"2001:4860::1","public"},{"2001:db8::1","documentation"},{"fe80::1","link_local"},{"fd00::1","private"},{"::1","loopback"}})
      require(ice_address_summary(ip)["address_class"]==scope,"address classification mismatch");
    const std::string candidate="candidate:secret 1 UDP 100 100.64.0.2 44444 typ srflx ufrag secret";
    const auto summary=ice_candidate_summary(candidate);
    require(summary["candidate_type"]=="srflx"&&summary["address_class"]=="shared_cgnat", "candidate evidence missing");
    require(summary.dump().find("secret")==std::string::npos&&summary.dump().find("100.64")==std::string::npos,"diagnostic leaked address/ufrag");
    const std::string sdp="v=0\r\na=ice-ufrag:keep\r\na=candidate:a 1 UDP 100 2001:4860::1 5000 typ host\r\na=candidate:b 1 UDP 10 8.8.8.8 5001 typ relay\r\n";
    const auto direct=ice_direct_sdp(sdp);
    require(direct.find("typ relay")==std::string::npos&&direct.find("typ host")!=std::string::npos&&direct.find("ice-ufrag:keep")!=std::string::npos,"direct filter damaged fences or IPv6 host");
    require(ice_has_turn({{"ice_servers",{{{"urls",{"stun:example.test","turn:example.test"}}}}}}),"mixed ICE URL list lost TURN");
    std::cout<<"PASS direct budget, connected/failed transitions, bounded fallback, dual-stack diagnostics and SDP fences\n";
    return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
