// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// hmiapp/car_identity.hpp: the series and fuel of the car's coding bytes, the VIN in a CDS text, the vehicle id made
// from it, the log's masking, and the lines the app writes for Flare Drive. Every series the coding can name is one
// core/sdr_builder.cpp knows, so the line the app offers is one the service reads.
#include <stdio.h>
#include <unistd.h>

#include <set>
#include <string>

#include "../core/sdr_builder.hpp"
#include "../hmiapp/car_identity.hpp"

namespace {

using namespace evo::app;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

void seriesBytes() {
    CHECK(std::string(seriesOfCoding(0x14)) == "F20");
    CHECK(std::string(seriesOfCoding(0x2a)) == "G20");
    CHECK(std::string(seriesOfCoding(0x3c)) == "J29");
    CHECK(std::string(seriesOfCoding(0x81)) == "RR31");
    CHECK(!seriesOfCoding(0x00) && !seriesOfCoding(0x05) && !seriesOfCoding(0xff) && !seriesOfCoding(0x114));
    // All 90 named codes, each once, each in the service's table.
    std::set<std::string> codes;
    for (unsigned v = 0; v < 256; ++v) {
        if (const char* code = seriesOfCoding(v)) {
            CHECK(codes.insert(code).second);
            CHECK(aa::findCarSeries(code) != nullptr);
        }
    }
    CHECK(codes.size() == 90);
}

void fuelBytes() {
    CHECK(std::string(fuelOfCoding(1)) == "petrol");
    CHECK(std::string(fuelOfCoding(2)) == "diesel");
    CHECK(std::string(fuelOfCoding(4)) == "diesel");  // with AdBlue
    for (unsigned v : {0u, 3u, 5u, 6u, 8u, 255u}) {
        CHECK(!fuelOfCoding(v));
    }
}

void vins() {
    // Made up, in a VIN's form.
    const char* vin = "WBA1A11010J123456";
    CHECK(fullVin(vin) && !fullVin("WBA1A11010J12345") && !fullVin("WBA1A11010J1234567") &&
          !fullVin("WBA1A1101OJ123456") && !fullVin("wba1a11010j123456") && !fullVin(nullptr));
    char out[18];
    CHECK(vinInText("{\"VIN\":\"WBA1A11010J123456\"}", out) && std::string(out) == vin);
    CHECK(vinInText("WBA1A11010J123456", out) && std::string(out) == vin);
    CHECK(!vinInText("{\"VIN\":\"0J12345\"}", out) && out[0] == 0);             // the short form: no id
    CHECK(!vinInText("{\"VIN\":\"WBA1A11010J1234567\"}", out) && out[0] == 0);  // 18 characters
    CHECK(!vinInText("", out) && !vinInText(nullptr, out));
}

void vehicleIds() {
    char a[37], b[37], c[37];
    vehicleIdOfVin("WBA1A11010J123456", a);
    vehicleIdOfVin("WBA1A11010J123456", b);
    vehicleIdOfVin("WBA1A11010J123457", c);
    CHECK(aa::validVehicleId(a) && std::string(a) == b && std::string(a) != c);
    CHECK(a[14] == '5' && (a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b'));  // version 5, RFC 4122
    CHECK(std::string(a).find("123456") == std::string::npos);
}

void masking() {
    char out[64];
    maskForLog("{\"VIN\":\"WBA1A11010J123456\"}", out, sizeof(out));
    CHECK(std::string(out) == "{\"VIN\":\"WBA1A11010*******\"}");
    maskForLog("{\"type\":\"F20\",\"id\":7}", out, sizeof(out));
    CHECK(std::string(out) == "{\"type\":\"F20\",\"id\":7}");
    maskForLog("abcdefghijk", out, 6);  // cut to the buffer
    CHECK(std::string(out) == "abcde");
}

void carFile() {
    CarFacts f;
    CHECK(carFileText(f).empty());
    f.series = "F20";
    f.side = "left";
    f.fuel = "petrol";
    f.vehicleId = "0f1e2d3c-4b5a-5978-8796-a5b4c3d2e1f0";
    const std::string text = carFileText(f);
    CHECK(text == "series=F20\nid=0f1e2d3c-4b5a-5978-8796-a5b4c3d2e1f0\nside=left\nfuel=petrol\n");
    // The service reads the same lines as the vehicle file.
    char path[64];
    snprintf(path, sizeof(path), "/tmp/test-car-identity-%d", static_cast<int>(getpid()));
    FILE* file = fopen(path, "w");
    CHECK(file);
    if (file) {
        fputs(text.c_str(), file);
        fclose(file);
    }
    aa::SdrIdentity id;
    CHECK(id.loadVehicleFile(path) && id.carModel == "1 Series (F20)" && id.vehicleId == f.vehicleId &&
          !id.rightHandDrive && id.fuel == aa::kFuelPetrol);
    unlink(path);
}

}  // namespace

int main() {
    seriesBytes();
    fuelBytes();
    vins();
    vehicleIds();
    masking();
    carFile();
    if (g_failures) {
        printf("FAIL car identity: %d check(s)\n", g_failures);
        return 1;
    }
    puts(
        "PASS car identity: series and fuel of the coding bytes, the VIN in a CDS text, its vehicle id, the log's "
        "masking, the lines for Flare Drive read as the service reads them");
    return 0;
}
