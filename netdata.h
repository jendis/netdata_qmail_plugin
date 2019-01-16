enum nd_visibility {
	ND_VISIBLE = 0,
	ND_HIDDEN,
};

enum nd_algorithm {
	ND_ALG_ABSOLUTE,
	ND_ALG_INCREMENTAL,
	ND_PERCENTAGE_OF_ABSOLUTE_ROW,
	ND_PERCENTAGE_OF_INCREMENTAL_ROW,
};

enum nd_charttype {
	ND_CHART_TYPE_LINE,
	ND_CHART_TYPE_AREA,
	ND_CHART_TYPE_STACKED,
};

void nd_disable();

void nd_chart(
	const char * type_id,
	const char * name,
	const char * title,
	const char * units,
	const char * family,
	const char * context,
	enum nd_charttype charttype
);

void nd_dimension(
	const char * id,
	const char * name,
	enum nd_algorithm algorithm,
	int multiplier,
	int divisor,
	enum nd_visibility visibility
);

void nd_begin(const char *);
void nd_set(const char *, const long);
void nd_end();
