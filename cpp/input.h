#pragma once

#include "load.h"

#include <vector>

class Places : public VertexCollection<2> {
    std::unique_ptr<GrB_Index[]> indicesSortedByNames;

public:
    enum Type : unsigned char {
        Continent, Country, City
    };

    using VertexCollection::VertexCollection;

    std::vector<std::string> names;
    std::vector<Type> types;

    std::vector<std::string> extraColumns() const override {
        return {"name", ":LABEL"};
    }

    void importFile() override {
        VertexCollection::importFile();

        sortIndicesByAttribute(names, indicesSortedByNames);
    }

    bool parseLine(CsvReaderT &csv_reader, GrB_Index &id) override {
        using namespace std::literals;

        std::string name, type_str;
        if (csv_reader.read_row(id, name, type_str)) {
            names.push_back(std::move(name));

            Type type;
            if (type_str == "Continent")
                type = Continent;
            else if (type_str == "Country")
                type = Country;
            else
                type = City;
            types.push_back(type);

            return true;
        } else
            return false;
    }

    /// Find the minimum index of places having the given name
    GrB_Index findIndexByName(std::string const &name) const {
        return findIndexByAttributeValue(name, names, indicesSortedByNames);
    }
};

inline constexpr Places::Type &operator++(Places::Type &val) {
    switch (val) {
        case Places::Continent:
            val = Places::Country;
            break;
        case Places::Country:
            val = Places::City;
            break;
        case Places::City:
        default:
            throw std::runtime_error("There is no next place.");
    }
    return val;
}

struct Tags : public VertexCollection<1> {
    std::unique_ptr<GrB_Index[]> indicesSortedByNames;

public:
    using VertexCollection::VertexCollection;

    std::vector<std::string> names;

    std::vector<std::string> extraColumns() const override {
        return {"name"};
    }

    void importFile() override {
        VertexCollection::importFile();

        sortIndicesByAttribute(names, indicesSortedByNames);
    }

    bool parseLine(CsvReaderT &csv_reader, GrB_Index &id) override {
        std::string name;
        if (csv_reader.read_row(id, name)) {
            names.push_back(std::move(name));
            return true;
        } else
            return false;
    }

    GrB_Index findIndexByName(std::string const &name) const {
        return findIndexByAttributeValue(name, names, indicesSortedByNames);
    }
};

struct Persons : public VertexCollection<1> {
    using VertexCollection::VertexCollection;
    /// loaded as IDs, later transformed to indices when Person_IsLocatedIn_City edge is loaded
    std::vector<GrB_Index> cityIndices;

    std::vector<std::string> extraColumns() const override {
        return {":END_ID(Place)"};
    }

    const char *getIdFieldName() const override {
        return ":START_ID(Person)";
    }

    const char *getIdFieldPrefix() const override {
        return ":START_ID(";
    }

    bool parseLine(CsvReaderT &csv_reader, GrB_Index &id) override {
        GrB_Index place_id;
        if (csv_reader.read_row(id, place_id)) {
            cityIndices.push_back(place_id);
            return true;
        } else
            return false;
    }
};

struct PersonIsLocatedInCityTranEdgeCollection : public EdgeCollection {
    Persons &persons;
    Places const &places;

    PersonIsLocatedInCityTranEdgeCollection(Persons &persons, Places const &places)
            : EdgeCollection("", true), persons(persons), places(places) {}

    void importFile(const std::vector<std::reference_wrapper<BaseVertexCollection>> &vertex_collection) override {
        trg = &persons;
        src = &places;
        edgeNumber = persons.size();

        // convert city IDs to indices in persons
        for (int person_index = 0; person_index < persons.size(); ++person_index) {
            GrB_Index &city_index = persons.cityIndices[person_index];
            city_index = places.idToIndex(city_index);
        }

        matrix = GB(GrB_Matrix_new, GrB_BOOL, src->size(), trg->size());
        ok(GrB_Matrix_build_BOOL(matrix.get(),
                                 persons.cityIndices.data(), array_of_indices(edgeNumber).get(),
                                 array_of_true(edgeNumber).get(),
                                 edgeNumber, GrB_LOR));
    }
};

struct PersonsWithBirthdays : public VertexCollection<1> {
    using VertexCollection::VertexCollection;

    std::vector<time_t> birthdays;

    std::vector<std::string> extraColumns() const override {
        return {"birthday"};
    }

    bool parseLine(CsvReaderT &csv_reader, GrB_Index &id) override {
        const char *birthday_str = nullptr;
        if (!csv_reader.read_row(id, birthday_str))
            return false;

        birthdays.push_back(parseTimestamp(birthday_str, DateFormat));

        return true;
    }
};

struct Comments : public VertexCollection<1> {
    using VertexCollection::VertexCollection;
    /// loaded as IDs, later transformed to indices when hasCreator edge is loaded
    std::vector<GrB_Index> creatorPersonIndices;

    std::vector<std::string> extraColumns() const override {
        return {":END_ID(Person)"};
    }

    const char *getIdFieldName() const override {
        return ":START_ID(Comment)";
    }

    const char *getIdFieldPrefix() const override {
        return ":START_ID(";
    }

    bool parseLine(CsvReaderT &csv_reader, GrB_Index &id) override {
        GrB_Index creator_person_id;
        if (csv_reader.read_row(id, creator_person_id)) {
            creatorPersonIndices.push_back(creator_person_id);
            return true;
        } else
            return false;
    }
};

struct HasCreatorEdgeCollection : public EdgeCollection {
    Comments &comments;
    Persons const &persons;

    HasCreatorEdgeCollection(Comments &comments, Persons const &persons)
            : EdgeCollection("", false), comments(comments), persons(persons) {}

    void importFile(const std::vector<std::reference_wrapper<BaseVertexCollection>> &vertex_collection) override {
        src = &comments;
        trg = &persons;
        edgeNumber = comments.size();

        // convert person IDs to indices in comments
        for (int comment_index = 0; comment_index < comments.size(); ++comment_index) {
            GrB_Index &person_index = comments.creatorPersonIndices[comment_index];
            person_index = persons.idToIndex(person_index);
        }

        matrix = GB(GrB_Matrix_new, GrB_BOOL, src->size(), trg->size());
        ok(GrB_Matrix_build_BOOL(matrix.get(),
                                 array_of_indices(edgeNumber).get(), comments.creatorPersonIndices.data(),
                                 array_of_true(edgeNumber).get(),
                                 edgeNumber, GrB_LOR));
    }
};

struct Forums : public VertexCollection<0> {
    using VertexCollection::VertexCollection;

    bool parseLine(CsvReaderT &csv_reader, GrB_Index &id) override {
        return csv_reader.read_row(id);
    }
};

struct Organizations : public VertexCollection<1> {
    enum Type : unsigned char {
        University, Company
    };

    using VertexCollection::VertexCollection;
    /// loaded as IDs, later transformed to indices when Organization_IsLocatedIn_Place edge is loaded
    std::vector<GrB_Index> placeIndices;
    /// overwritten later when Organization_IsLocatedIn_Place edge is loaded
    std::vector<Type> types;

    std::vector<std::string> extraColumns() const override {
        return {":END_ID(Place)"};
    }

    const char *getIdFieldName() const override {
        return ":START_ID(Organisation)";
    }

    const char *getIdFieldPrefix() const override {
        return ":START_ID(";
    }

    void importFile() override {
        VertexCollection::importFile();

        types.resize(size());
    }

    bool parseLine(CsvReaderT &csv_reader, GrB_Index &id) override {
        GrB_Index place_id;
        if (csv_reader.read_row(id, place_id)) {
            placeIndices.push_back(place_id);
            return true;
        } else
            return false;
    }
};

struct OrganizationIsLocatedInPlaceTranEdgeCollection : public EdgeCollection {
    Organizations &organizations;
    Places const &places;

    OrganizationIsLocatedInPlaceTranEdgeCollection(Organizations &organizations, Places const &places)
            : EdgeCollection("", true), organizations(organizations), places(places) {}

    void importFile(const std::vector<std::reference_wrapper<BaseVertexCollection>> &vertex_collection) override {
        trg = &organizations;
        src = &places;
        edgeNumber = organizations.size();

        // convert place IDs to indices in organizations
        for (int organization_index = 0; organization_index < organizations.size(); ++organization_index) {
            GrB_Index &place_index = organizations.placeIndices[organization_index];
            place_index = places.idToIndex(place_index);

            // set Organization.type
            Organizations::Type type;
            if (places.types[place_index] == Places::Country)
                type = Organizations::Company;
            else
                type = Organizations::University;
            organizations.types[organization_index] = type;
        }

        matrix = GB(GrB_Matrix_new, GrB_BOOL, src->size(), trg->size());
        ok(GrB_Matrix_build_BOOL(matrix.get(),
                                 organizations.placeIndices.data(), array_of_indices(edgeNumber).get(),
                                 array_of_true(edgeNumber).get(),
                                 edgeNumber, GrB_LOR));
    }
};

struct QueryInput : public BaseQueryInput {
    Places places;
    Tags tags;
    Forums forums;
    Persons persons;
    PersonsWithBirthdays personsWithBirthdays;
    Comments comments;
    Organizations organizations;

    EdgeCollection knows;
    EdgeCollection hasInterestTran;
    HasCreatorEdgeCollection hasCreator;
    EdgeCollection replyOf;
    EdgeCollection hasTag;
    EdgeCollection hasMember;
    PersonIsLocatedInCityTranEdgeCollection personIsLocatedInCityTran;
    OrganizationIsLocatedInPlaceTranEdgeCollection organizationIsLocatedInPlaceTran;
    EdgeCollection isPartOfTran;
    EdgeCollection workAtTran;
    EdgeCollection studyAtTran;

    explicit QueryInput(const BenchmarkParameters &parameters) :
            places{parameters.CsvPath + "place.csv"},
            tags{parameters.CsvPath + "tag.csv"},
            forums{parameters.CsvPath + "forum.csv"},
            persons{parameters.CsvPath + "person_isLocatedIn_place.csv"},
            personsWithBirthdays{parameters.CsvPath + "person.csv"},
            comments{parameters.CsvPath + "comment_hasCreator_person.csv"},
            organizations{parameters.CsvPath + "organisation_isLocatedIn_place.csv"},

            knows{parameters.CsvPath + "person_knows_person.csv"},
            hasInterestTran{parameters.CsvPath + "person_hasInterest_tag.csv", true},
            hasCreator{comments, persons},
            replyOf{parameters.CsvPath + "comment_replyOf_comment.csv"},
            hasTag{parameters.CsvPath + "forum_hasTag_tag.csv"},
            hasMember{parameters.CsvPath + "forum_hasMember_person.csv"},
            personIsLocatedInCityTran{persons, places},
            organizationIsLocatedInPlaceTran{organizations, places},
            isPartOfTran{parameters.CsvPath + "place_isPartOf_place.csv", true},
            workAtTran{parameters.CsvPath + "person_workAt_organisation.csv", true},
            studyAtTran{parameters.CsvPath + "person_studyAt_organisation.csv", true} {
        switch (parameters.Query) {
            case 1:
                vertexCollections = {comments, persons};
                edgeCollections = {knows, hasCreator, replyOf};
                break;
            case 2:
                vertexCollections = {tags, personsWithBirthdays};
                edgeCollections = {knows, hasInterestTran};
                break;
            case 3:
                vertexCollections = {places, tags, persons, organizations};
                edgeCollections = {knows, hasInterestTran, personIsLocatedInCityTran,
                                   organizationIsLocatedInPlaceTran, isPartOfTran, workAtTran, studyAtTran};
                break;
            case 4:
                vertexCollections = {tags, forums, persons};
                edgeCollections = {knows, hasTag, hasMember};
                break;
            default:
                vertexCollections = {places, tags, forums, persons, personsWithBirthdays, comments, organizations};
                edgeCollections = {knows, hasInterestTran, hasCreator, replyOf, hasTag, hasMember,
                                   personIsLocatedInCityTran, organizationIsLocatedInPlaceTran,
                                   isPartOfTran, workAtTran, studyAtTran};
                break;
        }

        for (auto const &collection : vertexCollections) {
            collection.get().importFile();
        }
        for (auto const &collection : edgeCollections) {
            collection.get().importFile(vertexCollections);
        }
    }
};
