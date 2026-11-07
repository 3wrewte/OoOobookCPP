#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "io/csvloader.hpp"

namespace {

std::string write_fixture(const std::string& name, const std::string& body) {
    const auto dir = std::filesystem::path("build");
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    return path.string();
}

} // namespace

TEST_CASE("csv loader parses the full crosscheck schema") {
    const auto path = write_fixture(
        "csvloader_full.csv",
        "seq,op,order_id,user_id,contract_id,side,price,qty,order_type\n"
        "1,N,11,1001,2,buy,10000,7,limit\n"
        "2,C,11,1001,2,,,,\n"
        "3,N,12,1002,2,sell,10002,5,limit\n");

    InstructionsFromCSV csv(path);
    csv.reset();
    REQUIRE(csv.instructions.size() == 3);

    const auto& buy = csv.instructions[0];
    CHECK(buy.kind == OPKind::Limit);
    CHECK(buy.Seq == 1);
    CHECK(buy.ClientOrderId == 11);
    CHECK(buy.AccountId == 1001);
    CHECK(buy.ContractId == 2);
    CHECK(buy.Side == Side_t::Buy);
    CHECK(buy.Price == 10000);
    CHECK(buy.qty == 7);

    const auto& cancel = csv.instructions[1];
    CHECK(cancel.kind == OPKind::Cancel);
    CHECK(cancel.Seq == 2);
    CHECK(cancel.ClientOrderId == 11);
    CHECK(cancel.AccountId == 1001);
    CHECK(cancel.ContractId == 2);
    CHECK(cancel.qty == 0);

    const auto& sell = csv.instructions[2];
    CHECK(sell.kind == OPKind::Limit);
    CHECK(sell.ClientOrderId == 12);
    CHECK(sell.Side == Side_t::Sell);
    CHECK(sell.Price == 10002);
    CHECK(sell.qty == 5);
}

TEST_CASE("csv loader accepts aliases and applies Rust defaults") {
    const auto path = write_fixture(
        "csvloader_alias.csv",
        "client_order_id,account,action,bs,px,size\n"
        "42,7,new,B,123,9\n"
        "42,7,cancel,,,\n");

    InstructionsFromCSV csv(path);
    csv.reset();
    REQUIRE(csv.instructions.size() == 2);

    const auto& add = csv.instructions[0];
    CHECK(add.Seq == 1);           // default = data row number
    CHECK(add.ClientOrderId == 42);
    CHECK(add.AccountId == 7);
    CHECK(add.ContractId == 1);    // default contract
    CHECK(add.kind == OPKind::Limit); // default order type
    CHECK(add.Side == Side_t::Buy);
    CHECK(add.Price == 123);
    CHECK(add.qty == 9);

    CHECK(csv.instructions[1].kind == OPKind::Cancel);
    CHECK(csv.instructions[1].Seq == 2);
}

TEST_CASE("csv loader accepts market orders without a price") {
    const auto path = write_fixture(
        "csvloader_market.csv",
        "seq,op,order_id,user_id,contract_id,side,price,qty,order_type\n"
        "1,N,1,1001,1,buy,,5,market\n");

    InstructionsFromCSV csv(path);
    csv.reset();
    REQUIRE(csv.instructions.size() == 1);
    CHECK(csv.instructions[0].kind == OPKind::Market);
    CHECK(csv.instructions[0].Price == 0);
    CHECK(csv.instructions[0].qty == 5);
}

TEST_CASE("csv loader issue advances the cursor by the handshake size") {
    const auto path = write_fixture(
        "csvloader_issue.csv",
        "seq,op,order_id,user_id,contract_id,side,price,qty,order_type\n"
        "1,N,1,1001,1,buy,10000,7,limit\n"
        "2,N,2,1002,1,sell,10002,5,limit\n"
        "3,N,3,1003,1,buy,9999,4,limit\n");

    InstructionsFromCSV csv(path);
    csv.reset();
    REQUIRE(csv.get_valid_num() == 3);

    global_phase = Phase::Prepare;
    csv.prepare();
    global_phase = Phase::Comb;
    const auto first = csv.issue(2);
    CHECK(first.size() == 2);
    CHECK(first[0].ClientOrderId == 1);
    CHECK(first[1].ClientOrderId == 2);
    global_phase = Phase::Commit;
    csv.commit(); // the cursor only advances at commit

    CHECK(csv.get_valid_num() == 1);

    global_phase = Phase::Prepare;
    csv.prepare();
    global_phase = Phase::Comb;
    const auto rest = csv.issue(10); // capped by the remaining count
    CHECK(rest.size() == 1);
    CHECK(rest[0].ClientOrderId == 3);
    global_phase = Phase::Commit;
    csv.commit();

    CHECK(csv.get_valid_num() == 0);

    global_phase = Phase::Prepare;
    csv.prepare();
    global_phase = Phase::Comb;
    const auto empty = csv.issue(4);
    CHECK(empty.empty());
    global_phase = Phase::Commit;
    csv.commit();

    global_phase = Phase::Prepare; // leave the shared phase in its default state
}

TEST_CASE("csv loader rejects malformed rows with a line number") {
    const auto bad_side = write_fixture(
        "csvloader_bad_side.csv",
        "seq,op,order_id,user_id,contract_id,side,price,qty,order_type\n"
        "1,N,1,1001,1,sideways,10000,7,limit\n");
    InstructionsFromCSV a(bad_side);
    CHECK_THROWS_AS(a.reset(), std::runtime_error);

    const auto bad_price = write_fixture(
        "csvloader_bad_price.csv",
        "seq,op,order_id,user_id,contract_id,side,price,qty,order_type\n"
        "1,N,1,1001,1,buy,268435456,7,limit\n"); // 2^28
    InstructionsFromCSV b(bad_price);
    CHECK_THROWS_AS(b.reset(), std::runtime_error);

    const auto bad_contract = write_fixture(
        "csvloader_bad_contract.csv",
        "seq,op,order_id,user_id,contract_id,side,price,qty,order_type\n"
        "1,N,1,1001,128,buy,10000,7,limit\n");
    InstructionsFromCSV c(bad_contract);
    CHECK_THROWS_AS(c.reset(), std::runtime_error);

    const auto bad_action = write_fixture(
        "csvloader_bad_action.csv",
        "seq,op,order_id,user_id,contract_id,side,price,qty,order_type\n"
        "1,zzz,1,1001,1,buy,10000,7,limit\n");
    InstructionsFromCSV d(bad_action);
    CHECK_THROWS_AS(d.reset(), std::runtime_error);
}

TEST_CASE("csv loader reports a missing file") {
    InstructionsFromCSV csv("build/csvloader_does_not_exist.csv");
    CHECK_THROWS_AS(csv.reset(), std::runtime_error);
}

TEST_CASE("csv loader accepts a header-only file") {
    const auto path = write_fixture(
        "csvloader_header_only.csv",
        "seq,op,order_id,user_id,contract_id,side,price,qty,order_type\n");
    InstructionsFromCSV csv(path);
    csv.reset();
    CHECK(csv.instructions.empty());
    CHECK(csv.get_valid_num() == 0);
}
