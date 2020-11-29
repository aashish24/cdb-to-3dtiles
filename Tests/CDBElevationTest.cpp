#include "CDBElevation.h"
#include "CDBTo3DTiles.h"
#include "Config.h"
#include "catch2/catch.hpp"
#include "nlohmann/json.hpp"
#include <fstream>

using namespace CDBTo3DTiles;

static void checkAllConvertedImagery(const std::filesystem::path &imageryPath,
                                     const std::filesystem::path &imageryOutputPath,
                                     size_t expectedImageryCount)
{
    size_t textureCount = 0;
    for (std::filesystem::directory_entry levelDir : std::filesystem::directory_iterator(imageryPath)) {
        for (std::filesystem::directory_entry UREFDir : std::filesystem::directory_iterator(levelDir)) {
            for (std::filesystem::directory_entry tilePath : std::filesystem::directory_iterator(UREFDir)) {
                auto tileFilename = tilePath.path();
                if (tileFilename.extension() != ".jp2") {
                    continue;
                }

                auto texture = imageryOutputPath / (tileFilename.stem().string() + ".jpeg");
                REQUIRE(std::filesystem::exists(texture));
                ++textureCount;
            }
        }
    }

    REQUIRE(textureCount == expectedImageryCount);
}

static void checkElevationDuplicated(const std::filesystem::path &imageryPath,
                                     const std::filesystem::path &elevationPath,
                                     size_t expectedElevationCount)
{
    size_t elevationCount = 0;
    for (std::filesystem::directory_entry levelDir : std::filesystem::directory_iterator(imageryPath)) {
        for (std::filesystem::directory_entry UREFDir : std::filesystem::directory_iterator(levelDir)) {
            for (std::filesystem::directory_entry tilePath : std::filesystem::directory_iterator(UREFDir)) {
                if (tilePath.path().extension() != ".jp2") {
                    continue;
                }

                auto imageryTile = CDBTile::createFromFile(tilePath.path().stem().string());
                auto elevationTile = CDBTile(imageryTile->getGeoCell(),
                                             CDBDataset::Elevation,
                                             1,
                                             1,
                                             imageryTile->getLevel(),
                                             imageryTile->getUREF(),
                                             imageryTile->getRREF());
                REQUIRE(std::filesystem::exists(
                    elevationPath / (elevationTile.getRelativePath().stem().string() + ".b3dm")));

                ++elevationCount;
            }
        }
    }

    REQUIRE(elevationCount == expectedElevationCount);
}

static void checkUVTheSameAsOldElevation(const Mesh &subregionMesh,
                                         const Mesh &oldElevation,
                                         size_t gridWidth,
                                         glm::uvec2 gridFrom,
                                         glm::uvec2 gridTo)
{
    int UVidx = 0;
    for (uint32_t y = gridFrom.y; y < gridTo.y + 1; ++y) {
        for (uint32_t x = gridFrom.x; x < gridTo.x + 1; ++x) {
            size_t oldIdx = y * (gridWidth + 1) + x;
            REQUIRE(subregionMesh.UVs[UVidx].x == Approx(oldElevation.UVs[oldIdx].x));
            REQUIRE(subregionMesh.UVs[UVidx].y == Approx(oldElevation.UVs[oldIdx].y));
            ++UVidx;
        }
    }
}

static void checkUVReindexForSubRegion(const Mesh &subregionMesh, size_t gridWidth, size_t gridHeight)
{
    // check that UV is properly re-index from 0,0
    int UVidx = 0;
    for (size_t i = 0; i < gridHeight + 1; ++i) {
        for (size_t j = 0; j < gridWidth + 1; ++j) {
            REQUIRE(subregionMesh.UVs[UVidx].x
                    == Approx(static_cast<float>(j) / static_cast<float>(gridWidth)));
            REQUIRE(subregionMesh.UVs[UVidx].y
                    == Approx(static_cast<float>(i) / static_cast<float>(gridHeight)));
            ++UVidx;
        }
    }
}

TEST_CASE("Test create elevation from file", "[CDBElevation]")
{
    SECTION("Create from valid GeoTiff file")
    {
        auto elevation = CDBElevation::createFromFile(dataPath / "Elevation"
                                                      / "N34W119_D001_S001_T001_LC06_U0_R0.tif");
        REQUIRE(elevation != std::nullopt);
        REQUIRE(elevation->getGridWidth() == 16);
        REQUIRE(elevation->getGridHeight() == 16);

        // Level -6 has 16x16 raster but we extends to the edge to cover crack,
        // so total of vertices are 17x17 vertices
        const auto &mesh = elevation->getUniformGridMesh();
        REQUIRE(mesh.indices.size() == 16 * 16 * 6);
        REQUIRE(mesh.positions.size() == 289);
        REQUIRE(mesh.positionRTCs.size() == 289);
        REQUIRE(mesh.UVs.size() == 289);
        REQUIRE(mesh.normals.size() == 0);

        // Check tile
        const auto &cdbTile = elevation->getTile();
        REQUIRE(cdbTile.getGeoCell() == CDBGeoCell(34, -119));
        REQUIRE(cdbTile.getDataset() == CDBDataset::Elevation);
        REQUIRE(cdbTile.getCS_1() == 1);
        REQUIRE(cdbTile.getCS_2() == 1);
        REQUIRE(cdbTile.getLevel() == -6);
        REQUIRE(cdbTile.getUREF() == 0);
        REQUIRE(cdbTile.getRREF() == 0);
    }

    SECTION("Create from invalid name file")
    {
        auto elevation = CDBElevation::createFromFile(dataPath / "Elevation" / "InvalidName.tif");
        REQUIRE(elevation == std::nullopt);
    }

    SECTION("Create from valid name file but wrong extension")
    {
        auto elevation = CDBElevation::createFromFile(dataPath / "Elevation"
                                                      / "N34W119_D001_S001_T001_LC06_U0_R0.png");
        REQUIRE(elevation == std::nullopt);
    }

    SECTION("Create from valid name file but wrong component selectors")
    {
        auto elevation = CDBElevation::createFromFile(dataPath / "Elevation"
                                                      / "N34W119_D001_S001_T002_LC06_U0_R0.tif");
        REQUIRE(elevation == std::nullopt);
    }

    SECTION("Create from valid name file but invalid content")
    {
        auto elevation = CDBElevation::createFromFile(dataPath / "Elevation"
                                                      / "N34W119_D001_S001_T001_L06_U0_R0.tif");
        REQUIRE(elevation == std::nullopt);
    }
}

TEST_CASE("Test create sub region of an elevation", "[CDBElevation]")
{
    // 16x16 mesh
    auto elevation = CDBElevation::createFromFile(dataPath / "Elevation"
                                                  / "N34W119_D001_S001_T001_LC06_U0_R0.tif");
    REQUIRE(elevation != std::nullopt);

    SECTION("Create North West")
    {
        {
            // quarter of the grid is 8x8, but we extend to the edge to cover crack, so
            // actual vertices are 9x9 vertices
            auto NW = elevation->createNorthWestSubRegion(false);
            REQUIRE(NW != std::nullopt);
            REQUIRE(NW->getGridWidth() == 8);
            REQUIRE(NW->getGridHeight() == 8);

            const auto &mesh = NW->getUniformGridMesh();
            REQUIRE(mesh.indices.size() == 8 * 8 * 6);
            REQUIRE(mesh.positions.size() == 81);
            REQUIRE(mesh.positionRTCs.size() == 81);
            REQUIRE(mesh.UVs.size() == 81);

            // check that UV is still the same as the old elevation
            glm::uvec2 gridFrom(0, 0);
            glm::uvec2 gridTo(elevation->getGridWidth() / 2, elevation->getGridHeight() / 2);
            checkUVTheSameAsOldElevation(mesh,
                                         elevation->getUniformGridMesh(),
                                         elevation->getGridWidth(),
                                         gridFrom,
                                         gridTo);
        }

        {
            // quarter of the grid is 8x8, but we extend to the edge to cover crack, so
            // actual vertices are 9x9 vertices
            auto NW = elevation->createNorthWestSubRegion(true);
            REQUIRE(NW != std::nullopt);
            REQUIRE(NW->getGridWidth() == 8);
            REQUIRE(NW->getGridHeight() == 8);

            const auto &mesh = NW->getUniformGridMesh();
            REQUIRE(mesh.indices.size() == 8 * 8 * 6);
            REQUIRE(mesh.positions.size() == 81);
            REQUIRE(mesh.positionRTCs.size() == 81);
            REQUIRE(mesh.UVs.size() == 81);

            // check that UV is properly re-index from 0,0
            checkUVReindexForSubRegion(mesh, NW->getGridWidth(), NW->getGridHeight());
        }
    }

    SECTION("Create North East")
    {
        {
            // quarter of the grid is 8x8, but we extend to the edge to cover crack, so
            // actual vertices are 9x9 vertices
            auto NE = elevation->createNorthEastSubRegion(false);
            REQUIRE(NE != std::nullopt);
            REQUIRE(NE->getGridWidth() == 8);
            REQUIRE(NE->getGridHeight() == 8);

            const auto &mesh = NE->getUniformGridMesh();
            REQUIRE(mesh.indices.size() == 8 * 8 * 6);
            REQUIRE(mesh.positions.size() == 81);
            REQUIRE(mesh.positionRTCs.size() == 81);
            REQUIRE(mesh.UVs.size() == 81);

            // check that UV is still the same as the old elevation
            glm::uvec2 gridFrom(elevation->getGridWidth() / 2, 0);
            glm::uvec2 gridTo = gridFrom
                                + glm::uvec2(elevation->getGridWidth() / 2, elevation->getGridHeight() / 2);
            checkUVTheSameAsOldElevation(mesh,
                                         elevation->getUniformGridMesh(),
                                         elevation->getGridWidth(),
                                         gridFrom,
                                         gridTo);
        }

        {
            // quarter of the grid is 8x8, but we extend to the edge to cover crack, so
            // actual vertices are 9x9 vertices
            auto NE = elevation->createNorthEastSubRegion(true);
            REQUIRE(NE != std::nullopt);
            REQUIRE(NE->getGridWidth() == 8);
            REQUIRE(NE->getGridHeight() == 8);

            const auto &mesh = NE->getUniformGridMesh();
            REQUIRE(mesh.indices.size() == 8 * 8 * 6);
            REQUIRE(mesh.positions.size() == 81);
            REQUIRE(mesh.positionRTCs.size() == 81);
            REQUIRE(mesh.UVs.size() == 81);

            // check that UV is properly re-index from 0,0
            checkUVReindexForSubRegion(mesh, NE->getGridWidth(), NE->getGridHeight());
        }
    }

    SECTION("Create South West")
    {
        {
            // quarter of the grid is 8x8, but we extend to the edge to cover crack, so
            // actual vertices are 9x9 vertices
            auto SW = elevation->createSouthWestSubRegion(false);
            REQUIRE(SW != std::nullopt);
            REQUIRE(SW->getGridWidth() == 8);
            REQUIRE(SW->getGridHeight() == 8);

            const auto &mesh = SW->getUniformGridMesh();
            REQUIRE(mesh.indices.size() == 8 * 8 * 6);
            REQUIRE(mesh.positions.size() == 81);
            REQUIRE(mesh.positionRTCs.size() == 81);
            REQUIRE(mesh.UVs.size() == 81);

            // check that UV is still the same as the old elevation
            glm::uvec2 gridFrom(0, elevation->getGridHeight() / 2);
            glm::uvec2 gridTo = gridFrom
                                + glm::uvec2(elevation->getGridWidth() / 2, elevation->getGridHeight() / 2);
            checkUVTheSameAsOldElevation(mesh,
                                         elevation->getUniformGridMesh(),
                                         elevation->getGridWidth(),
                                         gridFrom,
                                         gridTo);
        }

        {
            // quarter of the grid is 8x8, but we extend to the edge to cover crack, so
            // actual vertices are 9x9 vertices
            auto SW = elevation->createSouthWestSubRegion(true);
            REQUIRE(SW != std::nullopt);
            REQUIRE(SW->getGridWidth() == 8);
            REQUIRE(SW->getGridHeight() == 8);

            const auto &mesh = SW->getUniformGridMesh();
            REQUIRE(mesh.indices.size() == 8 * 8 * 6);
            REQUIRE(mesh.positions.size() == 81);
            REQUIRE(mesh.positionRTCs.size() == 81);
            REQUIRE(mesh.UVs.size() == 81);

            // check that UV is properly re-index from 0,0
            checkUVReindexForSubRegion(mesh, SW->getGridWidth(), SW->getGridHeight());
        }
    }

    SECTION("Create South East")
    {
        {
            // quarter of the grid is 8x8, but we extend to the edge to cover crack, so
            // actual vertices are 9x9 vertices
            auto SE = elevation->createSouthEastSubRegion(false);
            REQUIRE(SE != std::nullopt);
            REQUIRE(SE->getGridWidth() == 8);
            REQUIRE(SE->getGridHeight() == 8);

            const auto &mesh = SE->getUniformGridMesh();
            REQUIRE(mesh.indices.size() == 8 * 8 * 6);
            REQUIRE(mesh.positions.size() == 81);
            REQUIRE(mesh.positionRTCs.size() == 81);
            REQUIRE(mesh.UVs.size() == 81);

            // check that UV is still the same as the old elevation
            glm::uvec2 gridFrom(elevation->getGridWidth() / 2, elevation->getGridHeight() / 2);
            glm::uvec2 gridTo = glm::uvec2(elevation->getGridWidth(), elevation->getGridHeight());
            checkUVTheSameAsOldElevation(mesh,
                                         elevation->getUniformGridMesh(),
                                         elevation->getGridWidth(),
                                         gridFrom,
                                         gridTo);
        }

        {
            // quarter of the grid is 8x8, but we extend to the edge to cover crack, so
            // actual vertices are 9x9 vertices
            auto SE = elevation->createSouthEastSubRegion(true);
            REQUIRE(SE != std::nullopt);
            REQUIRE(SE->getGridWidth() == 8);
            REQUIRE(SE->getGridHeight() == 8);

            const auto &mesh = SE->getUniformGridMesh();
            REQUIRE(mesh.indices.size() == 8 * 8 * 6);
            REQUIRE(mesh.positions.size() == 81);
            REQUIRE(mesh.positionRTCs.size() == 81);
            REQUIRE(mesh.UVs.size() == 81);

            // check that UV is properly re-index from 0,0
            checkUVReindexForSubRegion(mesh, SE->getGridWidth(), SE->getGridHeight());
        }
    }
}

TEST_CASE("Test conversion when elevation has more LOD than imagery", "[CDBElevationConversion]")
{
    SECTION("Imagery has only negative LOD")
    {
        std::filesystem::path input = dataPath / "ElevationMoreLODNegativeImagery";
        std::filesystem::path output = "ElevationMoreLODNegativeImagery";
        std::filesystem::path elevationOutputDir = output / "Tiles" / "N32" / "W118" / "Elevation" / "1_1";

        {
			Converter converter(input, output);
			converter.convert();

			// check that all the imagery created as textures
			std::filesystem::path textureOutputDir = elevationOutputDir / "Textures";
			REQUIRE(std::filesystem::exists(textureOutputDir));
			checkAllConvertedImagery(input / "Tiles" / "N32" / "W118" / "004_Imagery", textureOutputDir, 10);

			// verified tileset.json
			std::filesystem::path tilesetPath = elevationOutputDir / "tileset.json";
			REQUIRE(std::filesystem::exists(tilesetPath));

			std::ifstream verifiedJS(input / "VerifiedTileset.json");
			nlohmann::json verifiedJson = nlohmann::json::parse(verifiedJS);

			std::ifstream testJS(tilesetPath);
			nlohmann::json testJson = nlohmann::json::parse(testJS);

			REQUIRE(testJson == verifiedJson);
        }

        // remove the test output
        std::filesystem::remove_all(output);
    }

    SECTION("Imagery has positive LOD")
    {
        std::filesystem::path input = dataPath / "ElevationMoreLODPositiveImagery";
        std::filesystem::path output = "ElevationMoreLODPositiveImagery";
        std::filesystem::path elevationOutputDir = output / "Tiles" / "N32" / "W118" / "Elevation" / "1_1";

        {
			Converter converter(input, output);
			converter.convert();

			// check all the imagery are created
			std::filesystem::path textureOutputDir = elevationOutputDir / "Textures";
			REQUIRE(std::filesystem::exists(textureOutputDir));
			checkAllConvertedImagery(input / "Tiles" / "N32" / "W118" / "004_Imagery", textureOutputDir, 13);

			// verified tileset.json
			std::filesystem::path tilesetPath = elevationOutputDir / "tileset.json";
			REQUIRE(std::filesystem::exists(tilesetPath));

			std::ifstream verifiedJS(input / "VerifiedTileset.json");
			nlohmann::json verifiedJson = nlohmann::json::parse(verifiedJS);

			std::ifstream testJS(tilesetPath);
			nlohmann::json testJson = nlohmann::json::parse(testJS);

			REQUIRE(testJson == verifiedJson);
        }

        // remove the test output
        std::filesystem::remove_all(output);
    }
}

TEST_CASE("Test conversion when imagery has more LOD than elevation", "[CDBElevationConversion]")
{
    SECTION("Test conversion when elevation has negative LOD only")
    {
        std::filesystem::path input = dataPath / "ImageryMoreLODNegativeElevation";
        std::filesystem::path output = "ImageryMoreLODNegativeElevation";
        std::filesystem::path elevationOutputDir = output / "Tiles" / "N32" / "W118" / "Elevation" / "1_1";

        {
			Converter converter(input, output);
			converter.convert();

			// check all the imagery are created.
			// check that elevation is duplicated to levels where imagery exists
			std::filesystem::path imageryInput = input / "Tiles" / "N32" / "W118" / "004_Imagery";
			std::filesystem::path textureOutputDir = elevationOutputDir / "Textures";
			REQUIRE(std::filesystem::exists(textureOutputDir));
			checkAllConvertedImagery(imageryInput, textureOutputDir, 13);
			checkElevationDuplicated(imageryInput, elevationOutputDir, 13);

			// verified tileset.json
			std::filesystem::path tilesetPath = elevationOutputDir / "tileset.json";
			REQUIRE(std::filesystem::exists(tilesetPath));

			std::ifstream verifiedJS(input / "VerifiedTileset.json");
			nlohmann::json verifiedJson = nlohmann::json::parse(verifiedJS);

			std::ifstream testJS(tilesetPath);
			nlohmann::json testJson = nlohmann::json::parse(testJS);

			REQUIRE(testJson == verifiedJson);
        }

        // remove the test output
        std::filesystem::remove_all(output);
    }

    SECTION("Test conversion when elevation has positive LOD")
    {
        std::filesystem::path input = dataPath / "ImageryMoreLODPositiveElevation";
        std::filesystem::path output = "ImageryMoreLODPositiveElevation";
        std::filesystem::path elevationOutputDir = output / "Tiles" / "N32" / "W118" / "Elevation" / "1_1";

        {
			Converter converter(input, output);
			converter.convert();

			// check all the imagery are created.
			// check that elevation is duplicated to levels where imagery exists
			std::filesystem::path imageryInput = input / "Tiles" / "N32" / "W118" / "004_Imagery";
			std::filesystem::path textureOutputDir = elevationOutputDir / "Textures";
			REQUIRE(std::filesystem::exists(textureOutputDir));
			checkAllConvertedImagery(imageryInput, textureOutputDir, 18);
			checkElevationDuplicated(imageryInput, elevationOutputDir, 18);

			// verified tileset.json
			std::filesystem::path tilesetPath = elevationOutputDir / "tileset.json";
			REQUIRE(std::filesystem::exists(tilesetPath));

			std::ifstream verifiedJS(input / "VerifiedTileset.json");
			nlohmann::json verifiedJson = nlohmann::json::parse(verifiedJS);

			std::ifstream testJS(tilesetPath);
			nlohmann::json testJson = nlohmann::json::parse(testJS);

			REQUIRE(testJson == verifiedJson);
        }

        // remove the test output
        std::filesystem::remove_all(output);
    }
}

TEST_CASE("Test conversion using elevation LOD only", "[CDBElevationConversion]")
{
    std::filesystem::path input = dataPath / "ImageryMoreLODPositiveElevation";
    std::filesystem::path output = "ImageryMoreLODPositiveElevation";
    std::filesystem::path elevationOutputDir = output / "Tiles" / "N32" / "W118" / "Elevation" / "1_1";

    {
		Converter converter(input, output);
		converter.setElevationLODOnly(true);
		converter.convert();

		// elevation max level is 1, so we check that
		int maxLevel = -10;
		for (std::filesystem::directory_entry entry : std::filesystem::directory_iterator(elevationOutputDir)) {
			auto tile = CDBTile::createFromFile(entry.path().filename().string());
			if (tile) {
				maxLevel = glm::max(tile->getLevel(), maxLevel);
			}
		}

		REQUIRE(maxLevel == 1);
    }

    // remove the test output
    std::filesystem::remove_all(output);
}
